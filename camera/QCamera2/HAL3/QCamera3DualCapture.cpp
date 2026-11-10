/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3DualCapture.h"
#include <cmath>
#include <cerrno>
#ifdef __ANDROID__
#define LOG_TAG "DUALB"
#include <log/log.h>
#define DUALB_LOGI(...) ALOGI(__VA_ARGS__)
#else
#define DUALB_LOGI(...) do {} while (0)
#endif
#include <cstdlib>
#include <cstring>
#include <new>
#include <time.h>

namespace qcamera {
namespace {
constexpr uint64_t kMaxPairBytes = 96U * 1024U * 1024U;
int64_t nowNs() {
    timespec time = {};
    clock_gettime(CLOCK_MONOTONIC, &time);
    return int64_t(time.tv_sec) * 1000000000LL + time.tv_nsec;
}
bool validRect(const QCamera3DualCapture::Rect &r) {
    return r.left >= 0 && r.top >= 0 && r.width > 0 && r.height > 0 &&
            int64_t(r.left) + r.width <= INT32_MAX && int64_t(r.top) + r.height <= INT32_MAX;
}
bool sameRect(const QCamera3DualCapture::Rect &a, const QCamera3DualCapture::Rect &b) {
    return a.left == b.left && a.top == b.top && a.width == b.width && a.height == b.height;
}
void copyImage(const QCamera3DualCapture::Image &from, const QCamera3DualCapture::Image &to) {
    for (uint32_t y = 0; y < from.height; ++y)
        memcpy(to.data + to.yOffset + size_t(y) * to.yStride,
                from.data + from.yOffset + size_t(y) * from.yStride, from.width);
    for (uint32_t y = 0; y < from.height / 2; ++y)
        memcpy(to.data + to.uvOffset + size_t(y) * to.uvStride,
                from.data + from.uvOffset + size_t(y) * from.uvStride, from.width);
}

uint8_t bilinear(const uint8_t *plane, uint32_t stride, uint32_t width, uint32_t height,
        int64_t x, int64_t y) {
    const uint32_t x0 = uint32_t(x >> 16), y0 = uint32_t(y >> 16);
    const uint32_t x1 = x0 + 1 < width ? x0 + 1 : x0;
    const uint32_t y1 = y0 + 1 < height ? y0 + 1 : y0;
    const uint32_t fx = uint32_t(x) & 0xffff, fy = uint32_t(y) & 0xffff;
    const uint32_t a = plane[size_t(y0) * stride + x0], b = plane[size_t(y0) * stride + x1];
    const uint32_t c = plane[size_t(y1) * stride + x0], d = plane[size_t(y1) * stride + x1];
    const uint64_t top = uint64_t(a) * (0x10000 - fx) + uint64_t(b) * fx;
    const uint64_t bottom = uint64_t(c) * (0x10000 - fx) + uint64_t(d) * fx;
    return uint8_t((top * (0x10000 - fy) + bottom * fy) >> 32);
}
uint8_t bilinearPair(const uint8_t *plane, uint32_t stride, uint32_t pairs,
        uint32_t height, int64_t pairX, int64_t y, uint32_t offset) {
    const uint32_t x0 = uint32_t(pairX >> 16), y0 = uint32_t(y >> 16);
    const uint32_t x1 = x0 + 1 < pairs ? x0 + 1 : x0;
    const uint32_t y1 = y0 + 1 < height ? y0 + 1 : y0;
    const uint32_t fx = uint32_t(pairX) & 0xffff, fy = uint32_t(y) & 0xffff;
    const uint32_t a = plane[size_t(y0) * stride + 2 * x0 + offset];
    const uint32_t b = plane[size_t(y0) * stride + 2 * x1 + offset];
    const uint32_t c = plane[size_t(y1) * stride + 2 * x0 + offset];
    const uint32_t d = plane[size_t(y1) * stride + 2 * x1 + offset];
    const uint64_t top = uint64_t(a) * (0x10000 - fx) + uint64_t(b) * fx;
    const uint64_t bottom = uint64_t(c) * (0x10000 - fx) + uint64_t(d) * fx;
    return uint8_t((top * (0x10000 - fy) + bottom * fy) >> 32);
}
// Centre-crop the source to the destination aspect and scale to destination.
bool copyScaled(const QCamera3DualCapture::Image &from,
        const QCamera3DualCapture::Image &to) {
    if (!from.width || !from.height || !to.width || !to.height) return false;
    uint32_t cropW = from.width, cropH = from.height;
    if (uint64_t(cropW) * to.height > uint64_t(cropH) * to.width)
        cropW = uint32_t(uint64_t(cropH) * to.width / to.height) & ~1U;
    else
        cropH = uint32_t(uint64_t(cropW) * to.height / to.width) & ~1U;
    if (!cropW || !cropH) return false;
    const uint32_t left = (from.width - cropW) / 2, top = (from.height - cropH) / 2;
    for (uint32_t y = 0; y < to.height; ++y) {
        const int64_t sy = (int64_t(top) << 16) +
                (int64_t(y) * cropH << 16) / to.height;
        for (uint32_t x = 0; x < to.width; ++x) {
            const int64_t sx = (int64_t(left) << 16) +
                    (int64_t(x) * cropW << 16) / to.width;
            to.data[to.yOffset + size_t(y) * to.yStride + x] =
                    bilinear(from.data + from.yOffset, from.yStride, from.width,
                        from.height, sx, sy);
        }
    }
    const uint32_t toPairs = to.width / 2;
    for (uint32_t y = 0; y < to.height / 2; ++y) {
        const int64_t sy = (int64_t(top / 2) << 16) +
                (int64_t(y) * (cropH / 2) << 16) / (to.height / 2);
        for (uint32_t pair = 0; pair < toPairs; ++pair) {
            const int64_t sx = (int64_t(left / 2) << 16) +
                    (int64_t(pair) * (cropW / 2) << 16) / toPairs;
            uint8_t *uv = to.data + to.uvOffset + size_t(y) * to.uvStride + 2 * pair;
            uv[0] = bilinearPair(from.data + from.uvOffset, from.uvStride,
                    from.width / 2, from.height / 2, sx, sy, 0);
            uv[1] = bilinearPair(from.data + from.uvOffset, from.uvStride,
                    from.width / 2, from.height / 2, sx, sy, 1);
        }
    }
    return true;
}
// Centre-crop the ARC output to the auxiliary FOV and scale it back to the
// destination. Bilinear is a transparent CPU substitute for stock VDUpscale;
// not bit-equivalent to the vendor path.
bool copyCropScaled(const QCamera3DualCapture::Image &from,
        const QCamera3DualCapture::Image &to, float ratio) {
    if (!(ratio > 0.0f) || ratio >= 1.0f || !from.width || !from.height) return false;
    const uint32_t cropW = (uint32_t(float(from.width) * ratio)) & ~1U;
    const uint32_t cropH = (uint32_t(float(from.height) * ratio)) & ~1U;
    if (!cropW || !cropH || cropW > from.width || cropH > from.height) return false;
    const uint32_t left = (from.width - cropW) / 2, top = (from.height - cropH) / 2;
    for (uint32_t y = 0; y < to.height; ++y) {
        const int64_t sy = (int64_t(top) << 16) + (int64_t(y) * cropH << 16) / to.height;
        for (uint32_t x = 0; x < to.width; ++x) {
            const int64_t sx = (int64_t(left) << 16) + (int64_t(x) * cropW << 16) / to.width;
            to.data[to.yOffset + size_t(y) * to.yStride + x] =
                    bilinear(from.data + from.yOffset, from.yStride, from.width,
                        from.height, sx, sy);
        }
    }
    const uint32_t toPairs = to.width / 2;
    for (uint32_t y = 0; y < to.height / 2; ++y) {
        const int64_t sy = (int64_t(top / 2) << 16) +
                (int64_t(y) * (cropH / 2) << 16) / (to.height / 2);
        for (uint32_t pair = 0; pair < toPairs; ++pair) {
            const int64_t sx = (int64_t(left / 2) << 16) +
                    (int64_t(pair) * (cropW / 2) << 16) / toPairs;
            uint8_t *uv = to.data + to.uvOffset + size_t(y) * to.uvStride + 2 * pair;
            uv[0] = bilinearPair(from.data + from.uvOffset, from.uvStride,
                    from.width / 2, from.height / 2, sx, sy, 0);
            uv[1] = bilinearPair(from.data + from.uvOffset, from.uvStride,
                    from.width / 2, from.height / 2, sx, sy, 1);
        }
    }
    return true;
}
struct OwnedImage {
    QCamera3DualCapture::Image image = {};
    uint8_t *storage = nullptr;
    ~OwnedImage() { free(storage); }
    uint64_t bytes() const { return uint64_t(image.width) * image.height * 3 / 2; }
    bool allocate(uint32_t width, uint32_t height) {
        const uint64_t bytes = uint64_t(width) * height * 3 / 2;
        if (!width || !height || (width | height) & 1 || bytes > kMaxPairBytes) return false;
        storage = static_cast<uint8_t *>(malloc(size_t(bytes)));
        if (!storage) return false;
        const size_t luma = size_t(width) * height;
        image = {storage, size_t(bytes), width, height, 0, luma, width, width, height, height / 2};
        return true;
    }
};
} // namespace
struct QCamera3DualCapture::Group {
    Config config;
    Request request;
    Calibration calibration;
    uint64_t token;
    int64_t deadline;
    Metadata metadata[2] = {};
    bool haveMetadata[2] = {}, haveImage[2] = {};
    bool processing = false;
    std::atomic<bool> cancelled{false};
    OwnedImage images[2];
    int index(uint32_t handle, uint32_t frame) const {
        for (int i = 0; i < 2; ++i)
            if (config.sensors[i].cameraHandle == handle && request.frameIds[i] == frame) return i;
        return -1;
    }
    bool alive() const { return !cancelled.load() && nowNs() <= deadline; }
    bool ready() const { return alive() && haveMetadata[0] && haveMetadata[1] && haveImage[0] && haveImage[1]; }
};
QCamera3DualCapture::~QCamera3DualCapture() { flush(); }
bool QCamera3DualCapture::resize(const Image &from, const Image &to, bool crop,
        float ratio) {
    if (!crop && from.width == to.width && from.height == to.height) {
        copyImage(from, to);
        return true;
    }
    return crop ? copyCropScaled(from, to, ratio) : copyScaled(from, to);
}
bool QCamera3DualCapture::validConfig(const Config &config) {
    if (!config.sensors[0].cameraHandle || !config.sensors[1].cameraHandle ||
            config.sensors[0].cameraHandle == config.sensors[1].cameraHandle ||
            !std::isfinite(config.wideMaxFov) || config.wideMaxFov <= 0 || config.wideMaxFov >= 180 ||
            !std::isfinite(config.teleMaxFov) || config.teleMaxFov < 0 ||
            config.teleMaxFov >= 180 ||
            (config.teleMaxFov > 0 && config.teleMaxFov > config.wideMaxFov) ||
            config.maxTimestampDeltaNs <= 0 || config.maxTimestampDeltaNs > 10000000 ||
            config.timeoutNs <= 0 || config.timeoutNs > 10000000000LL) return false;
    for (const auto &sensor : config.sensors)
        if (!sensor.referenceWidth || !sensor.referenceHeight ||
                sensor.referenceWidth > 16384 || sensor.referenceHeight > 16384 ||
                !validRect(sensor.referenceCrop) || !validRect(sensor.activeArrayCrop) ||
                ((sensor.imageWidth || sensor.imageHeight) &&
                    (!sensor.imageWidth || !sensor.imageHeight ||
                    (sensor.imageWidth | sensor.imageHeight) & 1 ||
                    sensor.imageWidth > sensor.referenceWidth || sensor.imageHeight > sensor.referenceHeight ||
                    uint64_t(sensor.imageWidth) * sensor.referenceHeight !=
                        uint64_t(sensor.imageHeight) * sensor.referenceWidth)) ||
                (sensor.requireStreamCrop && (!sensor.imageWidth ||
                    !validRect(sensor.streamCrop) || !validRect(sensor.streamRoiMap))) ||
                !sensor.sourceWidth || !sensor.sourceHeight ||
                (sensor.sourceWidth | sensor.sourceHeight) & 1 ||
                (int64_t(sensor.sourceWidth) * sensor.imageHeight >
                    int64_t(sensor.sourceHeight) * sensor.imageWidth ?
                 int64_t(sensor.sourceWidth) * sensor.imageHeight -
                    int64_t(sensor.sourceHeight) * sensor.imageWidth :
                 int64_t(sensor.sourceHeight) * sensor.imageWidth -
                    int64_t(sensor.sourceWidth) * sensor.imageHeight) * 100 >
                    int64_t(sensor.imageWidth) * sensor.imageHeight) return false;
    return true;
}
bool QCamera3DualCapture::mapFocus(const Rect &crop, uint32_t width, uint32_t height,
        int32_t x, int32_t y, int32_t &imageX, int32_t &imageY) {
    if (!validRect(crop) || !width || !height || width > 16384 || height > 16384 ||
            x < crop.left || y < crop.top || int64_t(x) >= int64_t(crop.left) + crop.width ||
            int64_t(y) >= int64_t(crop.top) + crop.height) return false;
    imageX = int64_t(x - crop.left) * width / crop.width;
    imageY = int64_t(y - crop.top) * height / crop.height;
    return true;
}
int32_t QCamera3DualCapture::begin(const Config &config, const Request &request,
        const Calibration &calibration, uint64_t &token) {
    token = 0;
    int32_t x, y;
    if (!validConfig(config) || calibration.source == Calibration::NONE ||
            !QCamera3DualBokehAlgo::validCalibration(calibration.data, sizeof(calibration.data)) ||
            request.blurLevel < 0 || request.blurLevel > QCamera3DualBokehAlgo::kMaxBlurLevel ||
            !mapFocus(config.sensors[0].requireStreamCrop ? config.sensors[0].streamRoiMap :
                    config.sensors[0].referenceCrop, config.sensors[0].referenceWidth,
                    config.sensors[0].referenceHeight, request.focusX, request.focusY, x, y))
        return -EINVAL;
    std::lock_guard<std::mutex> lock(mMutex);
    if (mGroup && mGroup->alive()) return -EBUSY;
    if (mNextToken == UINT64_MAX) return -EOVERFLOW;
    std::shared_ptr<Group> group(new(std::nothrow) Group);
    if (!group) return -ENOMEM;
    group->config = config; group->request = request; group->calibration = calibration;
    group->token = ++mNextToken; group->deadline = nowNs() + config.timeoutNs;
    if (mGroup) mGroup->cancelled.store(true);
    mGroup = group; token = group->token;
    return 0;
}
uint64_t QCamera3DualCapture::tokenFor(uint32_t handle, uint32_t frame) const {
    std::lock_guard<std::mutex> lock(mMutex);
    return mGroup && mGroup->alive() && mGroup->index(handle, frame) >= 0 ? mGroup->token : 0;
}
int32_t QCamera3DualCapture::submitMetadata(uint64_t token, uint32_t handle, uint32_t frame,
        const Metadata &metadata) {
    std::lock_guard<std::mutex> lock(mMutex);
    if (!mGroup || mGroup->token != token || !mGroup->alive()) return -ECANCELED;
    const int index = mGroup->index(handle, frame);
    if (index < 0) return -ENOENT;
    if (mGroup->processing || mGroup->haveMetadata[index]) return -EALREADY;
    DUALB_LOGI("DUALBDBG meta sensor=%d crop=%d,%d,%d,%d exp=%lld,%lld,%d,%lld "
            "stream=%d,%d,%d,%d/%d,%d,%d,%d",
            index, metadata.crop.left, metadata.crop.top, metadata.crop.width,
            metadata.crop.height,
            static_cast<long long>(metadata.timestampNs),
            static_cast<long long>(metadata.exposureNs), metadata.sensitivity,
            static_cast<long long>(metadata.frameDurationNs),
            mGroup->config.sensors[index].referenceCrop.left,
            mGroup->config.sensors[index].referenceCrop.top,
            mGroup->config.sensors[index].referenceCrop.width,
            mGroup->config.sensors[index].referenceCrop.height,
            metadata.streamCrop.left, metadata.streamCrop.top, metadata.streamCrop.width,
            metadata.streamCrop.height);
    // The paired auxiliary daemon does not run its own AE: exposure, sensitivity
    // and frame duration can legitimately be zero. ARC only consumes the
    // timestamp, the scaler crop/ROI and the two images, so do not reject a real
    // aux frame for absent 3A telemetry.
    const bool aux = index == 1;
    Metadata mapped = metadata;
    const Sensor &cfg = mGroup->config.sensors[index];
    if (cfg.metadataWidth && cfg.metadataHeight &&
            (metadata.crop.width != int32_t(cfg.sourceWidth) ||
             metadata.crop.height != int32_t(cfg.sourceHeight)) &&
            metadata.crop.width > 0 && metadata.crop.height > 0) {
        const float sx = float(cfg.sourceWidth) / cfg.metadataWidth;
        const float sy = float(cfg.sourceHeight) / cfg.metadataHeight;
        auto mapRect = [&](Rect &r) {
            r.left = int32_t(r.left * sx) & ~1;
            r.top = int32_t(r.top * sy) & ~1;
            r.width = int32_t(r.width * sx) & ~1;
            r.height = int32_t(r.height * sy) & ~1;
        };
        mapRect(mapped.crop);
        mapRect(mapped.streamCrop);
        mapRect(mapped.streamRoiMap);
        DUALB_LOGI("DUALBDBG meta map sensor=%d raw=%dx%d -> %dx%d",
                index, cfg.metadataWidth, cfg.metadataHeight,
                cfg.sourceWidth, cfg.sourceHeight);
    }
    if (metadata.timestampNs <= 0 ||
            (!aux && (metadata.exposureNs <= 0 || metadata.sensitivity <= 0 ||
                metadata.frameDurationNs < metadata.exposureNs)) ||
            (aux && metadata.frameDurationNs <= 0) ||
            !sameRect(mapped.crop, mGroup->config.sensors[index].referenceCrop) ||
            (mGroup->config.sensors[index].requireStreamCrop &&
                (!validRect(mapped.streamCrop) || !validRect(mapped.streamRoiMap)))) {
        DUALB_LOGI("DUALBDBG meta bits ts=%d exp=%d sens=%d dur=%d crop=%d stream=%d roi=%d",
                metadata.timestampNs > 0, metadata.exposureNs > 0,
                metadata.sensitivity > 0,
                metadata.frameDurationNs >= metadata.exposureNs,
                sameRect(mapped.crop, mGroup->config.sensors[index].referenceCrop),
                sameRect(mapped.streamCrop, mGroup->config.sensors[index].streamCrop),
                validRect(mapped.streamRoiMap));
        DUALB_LOGI("DUALBDBG meta reject sensor=%d cfgCrop=%d,%d,%d,%d cfgStream=%d,%d,%d,%d "
                "cfgRoi=%d,%d,%d,%d", index,
                mGroup->config.sensors[index].referenceCrop.left,
                mGroup->config.sensors[index].referenceCrop.top,
                mGroup->config.sensors[index].referenceCrop.width,
                mGroup->config.sensors[index].referenceCrop.height,
                mGroup->config.sensors[index].streamCrop.left,
                mGroup->config.sensors[index].streamCrop.top,
                mGroup->config.sensors[index].streamCrop.width,
                mGroup->config.sensors[index].streamCrop.height,
                mGroup->config.sensors[index].streamRoiMap.left,
                mGroup->config.sensors[index].streamRoiMap.top,
                mGroup->config.sensors[index].streamRoiMap.width,
                mGroup->config.sensors[index].streamRoiMap.height);
        return -EINVAL;
    }
    mGroup->metadata[index] = mapped; mGroup->haveMetadata[index] = true;
    return 0;
}
int32_t QCamera3DualCapture::submitImage(uint64_t token, uint32_t handle, uint32_t frame,
        const Image &image) {
    if (!QCamera3HdrAlgo::validImage(image)) return -EINVAL;
    std::lock_guard<std::mutex> lock(mMutex);
    if (!mGroup || mGroup->token != token || !mGroup->alive()) return -ECANCELED;
    const int index = mGroup->index(handle, frame);
    if (index < 0) return -ENOENT;
    if (mGroup->processing || mGroup->haveImage[index]) return -EALREADY;
    const Sensor &sensor = mGroup->config.sensors[index];
    DUALB_LOGI("DUALBDBG image in sensor=%d handle=0x%x frame=%u src=%ux%u expected=%ux%u",
            index, handle, frame, image.width, image.height,
            sensor.sourceWidth, sensor.sourceHeight);
    if (image.width != sensor.sourceWidth || image.height != sensor.sourceHeight ||
            uint64_t(image.width) * image.height * 3 / 2 +
                mGroup->images[1 - index].bytes() > kMaxPairBytes)
        return -EINVAL;
    if (!mGroup->images[index].allocate(sensor.imageWidth, sensor.imageHeight)) return -ENOMEM;
    const int64_t copyStart = nowNs();
    (void)copyStart;
    if (image.width == sensor.imageWidth && image.height == sensor.imageHeight) {
        copyImage(image, mGroup->images[index].image);
    } else if (!copyScaled(image, mGroup->images[index].image)) {
        return -EINVAL;
    }
    DUALB_LOGI("DUALBDBG scale cost sensor=%u ms=%lld", index,
            static_cast<long long>((nowNs() - copyStart) / 1000000));
    DUALB_LOGI("DUALBDBG image sensor=%u src=%ux%u dst=%ux%u upscale=%d",
            index, image.width, image.height, sensor.imageWidth, sensor.imageHeight,
            image.width != sensor.imageWidth || image.height != sensor.imageHeight);
    mGroup->haveImage[index] = true;
    return 0;
}
bool QCamera3DualCapture::isReady(uint64_t token) const {
    std::lock_guard<std::mutex> lock(mMutex);
    return mGroup && mGroup->token == token && mGroup->ready() && !mGroup->processing;
}
bool QCamera3DualCapture::hasActiveCapture() const {
    std::lock_guard<std::mutex> lock(mMutex);
    return mGroup && mGroup->alive();
}
int32_t QCamera3DualCapture::process(uint64_t token, const Image &destination) {
    if (!QCamera3HdrAlgo::validImage(destination)) return -EINVAL;
    std::shared_ptr<Group> group;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (!mGroup || mGroup->token != token || !mGroup->alive()) return -ECANCELED;
        if (!mGroup->ready()) return -EAGAIN;
        if (mGroup->processing) return -EBUSY;
        group = mGroup;
        if (destination.width != group->images[0].image.width ||
                destination.height != group->images[0].image.height) return -EINVAL;
        group->processing = true;
    }
    const int64_t a = group->metadata[0].timestampNs, b = group->metadata[1].timestampNs;
    int32_t rc = (a > b ? a - b : b - a) > group->config.maxTimestampDeltaNs ? -ERANGE : 0;
    OwnedImage output;
    if (rc == 0 && !output.allocate(destination.width, destination.height)) rc = -ENOMEM;
    QCamera3DualBokehAlgo::Focus focus = {0, 0, group->request.blurLevel};
    const auto &focusCrop = group->config.sensors[0].requireStreamCrop ?
            group->metadata[0].streamRoiMap : group->config.sensors[0].referenceCrop;
    if (rc == 0 && !mapFocus(focusCrop, destination.width,
            destination.height, group->request.focusX, group->request.focusY, focus.x, focus.y)) rc = -EINVAL;
    const auto &wide = group->config.sensors[0];
    const auto &tele = group->config.sensors[1];
    const QCamera3DualBokehAlgo::Geometry geometry = {wide.referenceWidth, wide.referenceHeight,
        tele.referenceWidth, tele.referenceHeight, group->config.wideMaxFov};
    DUALB_LOGI("DUALBDBG arc wide=%ux%u tele=%ux%u dst=%ux%u",
            group->images[0].image.width, group->images[0].image.height,
            group->images[1].image.width, group->images[1].image.height,
            destination.width, destination.height);
    if (rc == 0)
        rc = QCamera3DualBokehAlgo::process(group->images[0].image, group->images[1].image,
                output.image, geometry, focus, group->calibration.data,
                sizeof(group->calibration.data), &group->cancelled);
    std::lock_guard<std::mutex> lock(mMutex);
    if (mGroup != group || !group->alive()) rc = -ECANCELED;
    if (rc == 0) {
        bool cropped = false;
        const float wideFov = group->config.wideMaxFov;
        const float teleFov = group->config.teleMaxFov;
        if (teleFov > 0 && teleFov < wideFov) {
            const float halfPi = 3.14159265358979323846f / 2.0f;
            const float ratio = tanf(teleFov * halfPi / 180.0f) /
                    tanf(wideFov * halfPi / 180.0f);
            cropped = copyCropScaled(output.image, destination, ratio);
            DUALB_LOGI("DUALBDBG fov crop wide=%.2f tele=%.2f ratio=%.3f ok=%d",
                    wideFov, teleFov, ratio, cropped ? 1 : 0);
            if (!cropped) rc = -EINVAL;
        }
        if (rc == 0 && !cropped) copyImage(output.image, destination);
    }
    if (mGroup == group) mGroup.reset();
    return rc;
}
bool QCamera3DualCapture::cancel(uint64_t token) {
    std::lock_guard<std::mutex> lock(mMutex);
    if (!mGroup || mGroup->token != token) return false;
    mGroup->cancelled.store(true); mGroup.reset(); return true;
}
void QCamera3DualCapture::flush() {
    std::lock_guard<std::mutex> lock(mMutex);
    if (mGroup) mGroup->cancelled.store(true);
    mGroup.reset();
}
} // namespace qcamera
