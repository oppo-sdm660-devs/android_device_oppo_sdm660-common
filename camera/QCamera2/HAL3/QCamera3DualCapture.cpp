/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3DualCapture.h"
#include <cmath>
#include <cerrno>
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
bool QCamera3DualCapture::validConfig(const Config &config) {
    if (!config.sensors[0].cameraHandle || !config.sensors[1].cameraHandle ||
            config.sensors[0].cameraHandle == config.sensors[1].cameraHandle ||
            !std::isfinite(config.wideMaxFov) || config.wideMaxFov <= 0 || config.wideMaxFov >= 180 ||
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
                    !validRect(sensor.streamCrop) || !validRect(sensor.streamRoiMap)))) return false;
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
    if (metadata.timestampNs <= 0 || metadata.exposureNs <= 0 || metadata.sensitivity <= 0 ||
            metadata.frameDurationNs < metadata.exposureNs ||
            !sameRect(metadata.crop, mGroup->config.sensors[index].referenceCrop) ||
            (mGroup->config.sensors[index].requireStreamCrop &&
                (!sameRect(metadata.streamCrop, mGroup->config.sensors[index].streamCrop) ||
                !sameRect(metadata.streamRoiMap, mGroup->config.sensors[index].streamRoiMap)))) return -EINVAL;
    mGroup->metadata[index] = metadata; mGroup->haveMetadata[index] = true;
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
    if (image.width > sensor.referenceWidth || image.height > sensor.referenceHeight ||
            (sensor.imageWidth && (image.width != sensor.imageWidth || image.height != sensor.imageHeight)) ||
            uint64_t(image.width) * sensor.referenceHeight != uint64_t(image.height) * sensor.referenceWidth ||
            uint64_t(image.width) * image.height * 3 / 2 + mGroup->images[1 - index].bytes() > kMaxPairBytes)
        return -EINVAL;
    if (!mGroup->images[index].allocate(image.width, image.height)) return -ENOMEM;
    copyImage(image, mGroup->images[index].image); mGroup->haveImage[index] = true;
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
    if (rc == 0)
        rc = QCamera3DualBokehAlgo::process(group->images[0].image, group->images[1].image,
                output.image, geometry, focus, group->calibration.data,
                sizeof(group->calibration.data), &group->cancelled);
    std::lock_guard<std::mutex> lock(mMutex);
    if (mGroup != group || !group->alive()) rc = -ECANCELED;
    if (rc == 0) copyImage(output.image, destination);
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
