/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3DualCapture.h"
#include <cmath>
#include <cerrno>
#include <chrono>
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
#include <vector>
#include <utility>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

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

struct SampleAxis {
    uint32_t first, second, fraction;
};
SampleAxis sampleAxis(uint32_t origin, uint32_t extent, uint32_t outputSize,
        uint32_t index, uint32_t sourceSize) {
    const int64_t position = (int64_t(origin) << 16) +
            (int64_t(index) * extent << 16) / outputSize;
    const uint32_t first = uint32_t(position >> 16);
    return {first, first + 1 < sourceSize ? first + 1 : first,
        uint32_t(position) & 0xffff};
}
int32_t horizontalSample(const uint8_t *row, const SampleAxis &sample) {
    const int32_t first = row[sample.first];
    const int32_t second = row[sample.second];
    return (first << 16) + (second - first) * int32_t(sample.fraction);
}
struct HorizontalBlock {
    uint32_t base;
    uint8_t first[8], second[8];
    int32_t fractions[8];
    bool vectorized;
};
std::vector<HorizontalBlock> horizontalBlocks(const std::vector<SampleAxis> &samples,
        uint32_t sourceWidth) {
    std::vector<HorizontalBlock> blocks(samples.size() / 8);
    for (uint32_t i = 0; i < blocks.size(); ++i) {
        auto &block = blocks[i];
        const uint32_t begin = samples[i * 8].first;
        block.base = sourceWidth >= 16 && begin > sourceWidth - 16 ? sourceWidth - 16 : begin;
        block.vectorized = sourceWidth >= 16;
        for (uint32_t j = 0; j < 8; ++j) {
            const auto &sample = samples[i * 8 + j];
            if (sample.first < block.base || sample.second < block.base ||
                    sample.first - block.base >= 16 || sample.second - block.base >= 16)
                block.vectorized = false;
            block.first[j] = uint8_t(sample.first - block.base);
            block.second[j] = uint8_t(sample.second - block.base);
            block.fractions[j] = int32_t(sample.fraction);
        }
    }
    return blocks;
}
void horizontalRow(const uint8_t *source, int32_t *output,
        const std::vector<SampleAxis> &samples, const std::vector<HorizontalBlock> &blocks) {
    uint32_t x = 0;
    for (const auto &block : blocks) {
        (void)block;
#if defined(__ARM_NEON)
        if (block.vectorized) {
            const uint8x8x2_t table = {{vld1_u8(source + block.base), vld1_u8(source + block.base + 8)}};
            const uint16x8_t first = vmovl_u8(vtbl2_u8(table, vld1_u8(block.first)));
            const uint16x8_t second = vmovl_u8(vtbl2_u8(table, vld1_u8(block.second)));
            const int32x4_t a = vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(first)));
            const int32x4_t b = vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(first)));
            const int32x4_t c = vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(second)));
            const int32x4_t d = vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(second)));
            vst1q_s32(output + x, vmlaq_s32(vshlq_n_s32(a, 16), vsubq_s32(c, a), vld1q_s32(block.fractions)));
            vst1q_s32(output + x + 4, vmlaq_s32(vshlq_n_s32(b, 16), vsubq_s32(d, b), vld1q_s32(block.fractions + 4)));
        } else
#endif
        {
            for (uint32_t j = 0; j < 8; ++j) output[x + j] = horizontalSample(source, samples[x + j]);
        }
        x += 8;
    }
    for (; x < samples.size(); ++x) output[x] = horizontalSample(source, samples[x]);
}
uint8_t verticalSample(int32_t top, int32_t bottom, int32_t fraction) {
    // Horizontal values fit in 24 bits. This difference form keeps the exact
    // original 32 fractional bits with one signed 32x32 -> 64 product.
    return uint8_t(((int64_t(top) << 16) + int64_t(bottom - top) * fraction) >> 32);
}
void verticalRow(const int32_t *top, const int32_t *bottom, uint8_t *output,
        uint32_t width, int32_t fraction) {
    uint32_t x = 0;
#if defined(__ARM_NEON)
    for (; x + 8 <= width; x += 8) {
        const int32x4_t a = vld1q_s32(top + x), b = vld1q_s32(bottom + x);
        const int32x4_t c = vld1q_s32(top + x + 4), d = vld1q_s32(bottom + x + 4);
        const int32x4_t ab = vsubq_s32(b, a), cd = vsubq_s32(d, c);
        const int64x2_t p0 = vmlal_n_s32(vshll_n_s32(vget_low_s32(a), 16), vget_low_s32(ab), fraction);
        const int64x2_t p1 = vmlal_n_s32(vshll_n_s32(vget_high_s32(a), 16), vget_high_s32(ab), fraction);
        const int64x2_t p2 = vmlal_n_s32(vshll_n_s32(vget_low_s32(c), 16), vget_low_s32(cd), fraction);
        const int64x2_t p3 = vmlal_n_s32(vshll_n_s32(vget_high_s32(c), 16), vget_high_s32(cd), fraction);
        const uint32x4_t lo = vreinterpretq_u32_s32(vcombine_s32(vshrn_n_s64(p0, 32), vshrn_n_s64(p1, 32)));
        const uint32x4_t hi = vreinterpretq_u32_s32(vcombine_s32(vshrn_n_s64(p2, 32), vshrn_n_s64(p3, 32)));
        vst1_u8(output + x, vmovn_u16(vcombine_u16(vmovn_u32(lo), vmovn_u32(hi))));
    }
#endif
    for (; x < width; ++x) output[x] = verticalSample(top[x], bottom[x], fraction);
}
void scalePlane(const uint8_t *source, uint32_t sourceStride, uint32_t sourceHeight,
        uint8_t *destination, uint32_t destinationStride, uint32_t destinationHeight,
        uint32_t sourceWidth, uint32_t top, uint32_t cropHeight,
        const std::vector<SampleAxis> &horizontal) {
    const uint32_t width = uint32_t(horizontal.size());
    const auto blocks = horizontalBlocks(horizontal, sourceWidth);
    std::vector<int32_t> rows[2] = {std::vector<int32_t>(width), std::vector<int32_t>(width)};
    uint32_t cached[2] = {UINT32_MAX, UINT32_MAX};
    const auto readRow = [&](unsigned slot, uint32_t y) {
        const uint8_t *sourceRow = source + size_t(y) * sourceStride;
        horizontalRow(sourceRow, rows[slot].data(), horizontal, blocks);
        cached[slot] = y;
    };
    for (uint32_t y = 0; y < destinationHeight; ++y) {
        const auto vertical = sampleAxis(top, cropHeight, destinationHeight, y, sourceHeight);
        if (cached[0] != vertical.first) {
            if (cached[1] == vertical.first) {
                std::swap(rows[0], rows[1]);
                std::swap(cached[0], cached[1]);
            } else readRow(0, vertical.first);
        }
        if (vertical.second != vertical.first && cached[1] != vertical.second)
            readRow(1, vertical.second);
        const int32_t *row0 = rows[0].data();
        const int32_t *row1 = rows[vertical.second == vertical.first ? 0 : 1].data();
        uint8_t *output = destination + size_t(y) * destinationStride;
        verticalRow(row0, row1, output, width, int32_t(vertical.fraction));
    }
}
bool copyWindowScaled(const QCamera3DualCapture::Image &from,
        const QCamera3DualCapture::Image &to, uint32_t left, uint32_t top,
        uint32_t cropW, uint32_t cropH) {
    // Preserve the original 16.16 coordinates and truncation. X is independent
    // of the row, so its 64-bit division belongs outside the pixel loop.
    std::vector<SampleAxis> horizontal(to.width);
    for (uint32_t x = 0; x < to.width; ++x)
        horizontal[x] = sampleAxis(left, cropW, to.width, x, from.width);
    scalePlane(from.data + from.yOffset, from.yStride, from.height,
            to.data + to.yOffset, to.yStride, to.height, from.width, top, cropH, horizontal);
    for (uint32_t pair = 0; pair < to.width / 2; ++pair) {
        const auto sample = sampleAxis(left / 2, cropW / 2, to.width / 2, pair, from.width / 2);
        for (uint32_t component = 0; component < 2; ++component)
            horizontal[2 * pair + component] = {2 * sample.first + component,
                2 * sample.second + component, sample.fraction};
    }
    scalePlane(from.data + from.uvOffset, from.uvStride, from.height / 2,
            to.data + to.uvOffset, to.uvStride, to.height / 2, from.width, top / 2, cropH / 2, horizontal);
    return true;
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
    const uint32_t left = ((from.width - cropW) / 2) & ~1U;
    const uint32_t top = ((from.height - cropH) / 2) & ~1U;
    return copyWindowScaled(from, to, left, top, cropW, cropH);
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
    const uint32_t left = ((from.width - cropW) / 2) & ~1U;
    const uint32_t top = ((from.height - cropH) / 2) & ~1U;
    return copyWindowScaled(from, to, left, top, cropW, cropH);
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
    bool sourceComplete[2] = {};
    bool processing = false;
    std::atomic<bool> cancelled{false};
    OwnedImage images[2];
    int index(uint32_t handle, uint32_t frame) const {
        for (int i = 0; i < 2; ++i)
            if (config.sensors[i].cameraHandle == handle && request.frameIds[i] == frame) return i;
        return -1;
    }
    bool sourcesComplete() const {
        return sourceComplete[0] && sourceComplete[1] && haveMetadata[0] && haveMetadata[1];
    }
    bool alive() const { return !cancelled.load() && (sourcesComplete() || nowNs() <= deadline); }
    bool ready() const { return alive() && haveMetadata[0] && haveMetadata[1] && haveImage[0] && haveImage[1]; }
};
QCamera3DualCapture::~QCamera3DualCapture() {
    flush();
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mDeadlineStopping = true;
        mDeadlineCondition.notify_all();
    }
    if (mDeadlineStarted) pthread_join(mDeadlineThread, nullptr);
}

void QCamera3DualCapture::disarmDeadlineLocked(uint64_t token) {
    if (mDeadlineToken != token) return;
    mDeadlineToken = 0;
    mDeadlineWake = nullptr;
    mDeadlineContext = nullptr;
    mDeadlineCondition.notify_all();
}

void *QCamera3DualCapture::deadlineRoutine(void *context) {
    auto &capture = *static_cast<QCamera3DualCapture *>(context);
    std::unique_lock<std::mutex> lock(capture.mMutex);
    while (!capture.mDeadlineStopping) {
        capture.mDeadlineCondition.wait(lock, [&] {
            return capture.mDeadlineStopping || capture.mDeadlineToken != 0;
        });
        if (capture.mDeadlineStopping) break;
        const uint64_t token = capture.mDeadlineToken;
        const auto group = capture.mGroup;
        if (!group || group->token != token || group->cancelled.load() ||
                group->sourcesComplete()) {
            capture.disarmDeadlineLocked(token);
            continue;
        }
        const int64_t remaining = group->deadline - nowNs();
        if (remaining > 0) {
            capture.mDeadlineCondition.wait_for(lock, std::chrono::nanoseconds(remaining));
            continue;
        }
        const auto wake = capture.mDeadlineWake;
        void *owner = capture.mDeadlineContext;
        DUALB_LOGI("PAIR_SOURCE deadline expired token=%llu frames=%u/%u raw=%d/%d meta=%d/%d",
                static_cast<unsigned long long>(token), group->request.frameIds[0],
                group->request.frameIds[1], group->sourceComplete[0],
                group->sourceComplete[1], group->haveMetadata[0], group->haveMetadata[1]);
        capture.mExpiredToken = token;
        capture.disarmDeadlineLocked(token);
        capture.mCallbackToken = token;
        lock.unlock();
        wake(owner, token);
        lock.lock();
        capture.mCallbackToken = 0;
        capture.mDeadlineCondition.notify_all();
    }
    return nullptr;
}

int32_t QCamera3DualCapture::armDeadline(uint64_t token, DeadlineWake wake, void *context) {
    if (!wake || !context) return -EINVAL;
    std::lock_guard<std::mutex> lock(mMutex);
    if (!mGroup || mGroup->token != token || mGroup->cancelled.load()) return -ECANCELED;
    if (mGroup->sourcesComplete()) return 0;
    if (nowNs() > mGroup->deadline) return -ETIMEDOUT;
    if (mDeadlineToken == token) return -EALREADY;
    if (!mDeadlineStarted) {
        const int rc = pthread_create(&mDeadlineThread, nullptr, deadlineRoutine, this);
        if (rc != 0) return -rc;
        mDeadlineStarted = true;
    }
    mDeadlineToken = token;
    mDeadlineWake = wake;
    mDeadlineContext = context;
    mDeadlineCondition.notify_all();
    return 0;
}

bool QCamera3DualCapture::deadlineExpired(uint64_t token) const {
    std::lock_guard<std::mutex> lock(mMutex);
    return token != 0 && (mExpiredToken == token || (mGroup && mGroup->token == token &&
            !mGroup->sourcesComplete() && nowNs() > mGroup->deadline));
}

int32_t QCamera3DualCapture::markSourceComplete(uint64_t token, uint32_t handle,
        uint32_t frame) {
    std::lock_guard<std::mutex> lock(mMutex);
    if (!mGroup || mGroup->token != token || !mGroup->alive()) return -ECANCELED;
    const int index = mGroup->index(handle, frame);
    if (index < 0) return -ENOENT;
    mGroup->sourceComplete[index] = true;
    if (mGroup->sourcesComplete()) disarmDeadlineLocked(token);
    return 0;
}
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
                ((sensor.calibrationWindow.width || sensor.calibrationWindow.height) &&
                    (!validRect(sensor.calibrationWindow) ||
                     sensor.calibrationWindow.width != int32_t(sensor.referenceWidth) ||
                     sensor.calibrationWindow.height != int32_t(sensor.referenceHeight))) ||
                ((sensor.nativeReferenceWindow.width || sensor.nativeReferenceWindow.height) &&
                    (!sensor.requirePixelGeometry || !validRect(sensor.nativeReferenceWindow) ||
                     !validRect(sensor.calibrationWindow) ||
                     int64_t(sensor.nativeReferenceWindow.width) * sensor.calibrationWindow.height !=
                        int64_t(sensor.nativeReferenceWindow.height) * sensor.calibrationWindow.width)) ||
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
bool QCamera3DualCapture::framingCrop(const Config &config, PortraitFraming framing,
        uint32_t width, uint32_t height, Rect &crop) {
    crop = {};
    if (!width || !height || width > 16384 || height > 16384 || (width | height) & 1 ||
            (framing != PortraitFraming::Half && framing != PortraitFraming::Full)) return false;
    float ratio = 1.0f;
    if (framing == PortraitFraming::Half) {
        if (!std::isfinite(config.wideMaxFov) || !std::isfinite(config.teleMaxFov) ||
                config.wideMaxFov <= 0 || config.wideMaxFov >= 180 || config.teleMaxFov <= 0 ||
                config.teleMaxFov >= config.wideMaxFov) return false;
        constexpr float halfRadians = 3.14159265358979323846f / 360.0f;
        ratio = tanf(config.teleMaxFov * halfRadians) / tanf(config.wideMaxFov * halfRadians);
    }
    const uint32_t cropW = uint32_t(width * ratio) & ~1U;
    const uint32_t cropH = uint32_t(height * ratio) & ~1U;
    if (!cropW || !cropH) return false;
    crop = {int32_t(((width - cropW) / 2) & ~1U), int32_t(((height - cropH) / 2) & ~1U),
        int32_t(cropW), int32_t(cropH)};
    return true;
}
int32_t QCamera3DualCapture::begin(const Config &config, const Request &request,
        const Calibration &calibration, uint64_t &token) {
    token = 0;
    int32_t x, y;
    Rect crop;
    if (!validConfig(config) || calibration.source == Calibration::NONE ||
            !QCamera3DualBokehAlgo::validCalibration(calibration.data, sizeof(calibration.data)) ||
            request.blurLevel < 0 || request.blurLevel > QCamera3DualBokehAlgo::kMaxBlurLevel ||
            !framingCrop(config, request.framing, config.sensors[0].imageWidth,
                    config.sensors[0].imageHeight, crop) ||
            !mapFocus(request.focusInActiveArray ? config.sensors[0].activeArrayCrop :
                    config.sensors[0].requireStreamCrop ? config.sensors[0].streamRoiMap :
                    config.sensors[0].referenceCrop, config.sensors[0].referenceWidth,
                    config.sensors[0].referenceHeight, request.focusX, request.focusY, x, y))
        return -EINVAL;
    std::lock_guard<std::mutex> lock(mMutex);
    if (mGroup && (mGroup->alive() || mDeadlineToken == mGroup->token ||
            mCallbackToken == mGroup->token)) return -EBUSY;
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
                (!validRect(mapped.streamCrop) || !validRect(mapped.streamRoiMap))) ||
            (cfg.requirePixelGeometry && (metadata.sourceSofNs <= 0 ||
                !metadata.pixelGeometry.valid()))) {
        DUALB_LOGI("DUALBDBG meta bits ts=%d exp=%d sens=%d dur=%d crop=%d stream=%d roi=%d pixel=%d",
                metadata.timestampNs > 0, metadata.exposureNs > 0,
                metadata.sensitivity > 0,
                metadata.frameDurationNs >= metadata.exposureNs,
                sameRect(mapped.crop, mGroup->config.sensors[index].referenceCrop),
                sameRect(mapped.streamCrop, mGroup->config.sensors[index].streamCrop),
                validRect(mapped.streamRoiMap),
                !cfg.requirePixelGeometry || metadata.pixelGeometry.valid());
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
    if (cfg.requirePixelGeometry) {
        if (cfg.nativeReferenceWindow.width > 0) {
            const auto &geometry = metadata.pixelGeometry;
            const auto &calibration = cfg.calibrationWindow;
            const auto &native = cfg.nativeReferenceWindow;
            const double sx = geometry.sensor.crop.width / geometry.camif.input.width;
            const double sy = geometry.sensor.crop.height / geometry.camif.input.height;
            const QCamera3DualPixelGeometry::Rect reference = {double(calibration.left),
                double(calibration.top), double(calibration.width), double(calibration.height)};
            if (!QCamera3DualPixelGeometry::contains(geometry.camif.input, reference) ||
                    geometry.sensor.crop.left + calibration.left * sx != native.left ||
                    geometry.sensor.crop.top + calibration.top * sy != native.top ||
                    calibration.width * sx != native.width || calibration.height * sy != native.height) {
                DUALB_LOGI("PAIR_GEOMETRY reference units stale sensor=%d native=%u", index,
                        metadata.nativeFrame);
                return -ESTALE;
            }
        }
        const auto window = metadata.pixelGeometry.sensorWindow();
        (void)window;
        DUALB_LOGI("PAIR_GEOMETRY source sensor=%d native=%u sof=%lld scale=%.9f "
                "sensorWindow=%.6f,%.6f,%.6f,%.6f output=%ux%u", index,
                metadata.nativeFrame, (long long)metadata.sourceSofNs,
                metadata.pixelGeometry.ispScaleRatio, window.left, window.top,
                window.width, window.height, metadata.pixelGeometry.outputWidth,
                metadata.pixelGeometry.outputHeight);
    }
    mGroup->metadata[index] = mapped; mGroup->haveMetadata[index] = true;
    if (mGroup->sourcesComplete()) disarmDeadlineLocked(token);
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
    const uint32_t storedWidth = sensor.requirePixelGeometry ? image.width : sensor.imageWidth;
    const uint32_t storedHeight = sensor.requirePixelGeometry ? image.height : sensor.imageHeight;
    if (!mGroup->images[index].allocate(storedWidth, storedHeight)) return -ENOMEM;
    const int64_t copyStart = nowNs();
    (void)copyStart;
    if (image.width == storedWidth && image.height == storedHeight) {
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
    mGroup->sourceComplete[index] = true;
    if (mGroup->sourcesComplete()) disarmDeadlineLocked(token);
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
        if (destination.width != group->config.sensors[0].imageWidth ||
                destination.height != group->config.sensors[0].imageHeight) return -EINVAL;
        group->processing = true;
    }
    const int64_t a = group->metadata[0].timestampNs, b = group->metadata[1].timestampNs;
    int32_t rc = (a > b ? a - b : b - a) > group->config.maxTimestampDeltaNs ? -ERANGE : 0;
    DUALB_LOGI("PAIR_TIMING native=%u/%u exposureStartNs=%lld/%lld limitNs=%lld "
            "axiDoneNs=%lld/%lld status=%d", group->metadata[0].nativeFrame,
            group->metadata[1].nativeFrame, (long long)a, (long long)b,
            (long long)group->config.maxTimestampDeltaNs,
            (long long)group->metadata[0].sourceSofNs,
            (long long)group->metadata[1].sourceSofNs, rc);
    OwnedImage referenceImages[2];
    const Image *inputs[2] = {&group->images[0].image, &group->images[1].image};
    for (unsigned i = 0; rc == 0 && i < 2; ++i) {
        const auto &sensor = group->config.sensors[i];
        if (!sensor.requirePixelGeometry) continue;
        const auto &mapping = group->metadata[i].pixelGeometry;
        const auto &calibration = sensor.nativeReferenceWindow.width > 0 ?
                sensor.nativeReferenceWindow : sensor.calibrationWindow;
        const QCamera3DualPixelGeometry::Rect reference = calibration.width > 0 ?
                QCamera3DualPixelGeometry::Rect{double(calibration.left), double(calibration.top),
                    double(calibration.width), double(calibration.height)} :
                QCamera3DualPixelGeometry::Rect{0, 0, double(sensor.activeArrayCrop.width),
                    double(sensor.activeArrayCrop.height)};
        if (!QCamera3DualPixelGeometry::contains(mapping.sensorWindow(), reference)) {
            rc = -ENOTSUP;
        } else if (mapping.clockwiseRotation == 0 &&
                mapping.sensorWindow().left == reference.left &&
                mapping.sensorWindow().top == reference.top &&
                mapping.sensorWindow().width == reference.width &&
                mapping.sensorWindow().height == reference.height &&
                inputs[i]->width == sensor.imageWidth && inputs[i]->height == sensor.imageHeight) {
            // A full, already scaled CPP buffer needs no second pixel pass.
        } else if (!referenceImages[i].allocate(sensor.imageWidth, sensor.imageHeight)) {
            rc = -ENOMEM;
        } else {
            rc = mapping.referenceImage(*inputs[i], reference, referenceImages[i].image);
            if (rc == 0) inputs[i] = &referenceImages[i].image;
        }
        const auto window = mapping.sensorWindow();
        (void)window;
        DUALB_LOGI("PAIR_GEOMETRY sensor=%u native=%u sof=%lld sensorWindow=%.6f,%.6f,%.6f,%.6f "
                "reference=%.0fx%.0f source=%ux%u status=%d", i,
                group->metadata[i].nativeFrame, (long long)group->metadata[i].sourceSofNs,
                window.left, window.top, window.width, window.height,
                reference.width, reference.height, inputs[i]->width, inputs[i]->height, rc);
    }
    OwnedImage output;
    if (rc == 0 && !output.allocate(destination.width, destination.height)) rc = -ENOMEM;
    QCamera3DualBokehAlgo::Focus focus = {0, 0, group->request.blurLevel};
    const auto &focusCrop = group->request.focusInActiveArray ?
            group->config.sensors[0].activeArrayCrop : group->config.sensors[0].requireStreamCrop ?
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
        rc = QCamera3DualBokehAlgo::process(*inputs[0], *inputs[1],
                output.image, geometry, focus, group->calibration.data,
                sizeof(group->calibration.data), &group->cancelled);
    std::lock_guard<std::mutex> lock(mMutex);
    if (mGroup != group || !group->alive()) rc = -ECANCELED;
    if (rc == 0) {
        bool cropped = false;
        const float wideFov = group->config.wideMaxFov;
        const float teleFov = group->config.teleMaxFov;
        if (group->request.framing == PortraitFraming::Half) {
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
    if (mGroup == group) {
        disarmDeadlineLocked(token);
        mGroup.reset();
    }
    return rc;
}
bool QCamera3DualCapture::cancel(uint64_t token) {
    std::unique_lock<std::mutex> lock(mMutex);
    const bool matches = mGroup && mGroup->token == token;
    if (matches) {
        if (!mGroup->sourcesComplete() && nowNs() > mGroup->deadline) mExpiredToken = token;
        mGroup->cancelled.store(true);
        mGroup.reset();
    }
    disarmDeadlineLocked(token);
    mDeadlineCondition.wait(lock, [&] { return mCallbackToken != token; });
    return matches;
}
void QCamera3DualCapture::flush() {
    std::unique_lock<std::mutex> lock(mMutex);
    if (mGroup) mGroup->cancelled.store(true);
    mGroup.reset();
    disarmDeadlineLocked(mDeadlineToken);
    mDeadlineCondition.wait(lock, [&] { return mCallbackToken == 0; });
    mExpiredToken = 0;
}
} // namespace qcamera
