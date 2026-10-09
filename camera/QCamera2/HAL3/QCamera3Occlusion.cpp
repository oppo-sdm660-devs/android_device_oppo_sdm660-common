/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3Occlusion.h"

#include <algorithm>
#include <cmath>
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <new>
#include <pthread.h>
#include <stdint.h>

namespace qcamera {
namespace {
constexpr uint32_t kBlockGrid = 32;          // 32 x 32 block means
constexpr uint32_t kBlockSamples = 4;        // 4 x 4 samples per block <= 16384
constexpr size_t kMaxSamples = kBlockGrid * kBlockGrid * kBlockSamples * kBlockSamples;
constexpr unsigned kCancelBlocks = 64;       // cancellation granularity

bool isCancelled(const std::atomic<bool> *flag) { return flag && flag->load(); }
using Image = QCamera3Occlusion::Image;

bool planeValid(size_t offset, uint32_t stride, uint32_t scanline,
        uint32_t width, uint32_t rows, size_t length) {
    if (stride < width || scanline < rows || !rows) return false;
    const uint64_t end = uint64_t(offset) + uint64_t(stride) * (rows - 1) + width;
    return end >= offset && end <= length;
}
bool validImage(const Image &im) {
    if (!im.data || !im.width || !im.height || (im.width & 1) || (im.height & 1) ||
            im.width > INT32_MAX - 63 || im.height > INT32_MAX ||
            im.yStride > INT32_MAX || im.uvStride > INT32_MAX) return false;
    if (!planeValid(im.yOffset, im.yStride, im.yScanline, im.width, im.height, im.length) ||
            !planeValid(im.uvOffset, im.uvStride, im.uvScanline,
                im.width, im.height / 2, im.length)) return false;
    const uint64_t yEnd = uint64_t(im.yOffset) + uint64_t(im.yStride) * (im.height - 1) + im.width;
    const uint64_t uvEnd = uint64_t(im.uvOffset) + uint64_t(im.uvStride) * (im.height / 2 - 1) + im.width;
    return yEnd <= im.uvOffset || uvEnd <= im.yOffset;
}
int32_t clampScore(double value) {
    if (!(value > 0.0)) return 0;
    if (value > 1.0) return 1000;
    return int32_t(std::lround(value * 1000.0));
}
} // namespace

struct QCamera3Occlusion::Impl {
    int32_t occluded = 0;
    int32_t latchCount = 0, releaseCount = 0;
    size_t lastSamples = 0;
    mutable pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
};

QCamera3Occlusion::QCamera3Occlusion() : mImpl(new (std::nothrow) Impl) {}
QCamera3Occlusion::~QCamera3Occlusion() {
    if (mImpl) pthread_mutex_destroy(&mImpl->lock);
    delete mImpl;
}

const char *QCamera3Occlusion::name() { return "Occlusion"; }
bool QCamera3Occlusion::isAvailable() { return true; }

bool QCamera3Occlusion::stockCoreAvailable() {
    // The three real undecorated exports of libOccluDetect.so (readelf
    // --dyn-syms on stock/R11 and stock/R11s). Resolved lazily and released
    // immediately: this is a presence probe, never a call. Probed once per
    // process so the per-frame analysis path never pays for a dlopen.
    static const bool available = []() {
        void *library = dlopen("libOccluDetect.so", RTLD_NOW | RTLD_LOCAL);
        if (!library) return false;
        const bool found = dlsym(library, "stInitialize") &&
                dlsym(library, "camOcclusionDetect") && dlsym(library, "stRelease");
        dlclose(library);
        return found;
    }();
    return available;
}

bool QCamera3Occlusion::supports(uint32_t width, uint32_t height) {
    return width >= 2 && height >= 2 && !(width & 1) && !(height & 1) &&
            width <= kMaxDimension && height <= kMaxDimension;
}

bool QCamera3Occlusion::validOptions(const Options &o) {
    if (o.threshold < 1 || o.threshold > 100) return false;
    if (o.releaseThreshold < 0 || o.releaseThreshold > o.threshold) return false;
    if (o.latchFrames < 1 || o.latchFrames > kMaxHysteresisFrames) return false;
    if (o.releaseFrames < 1 || o.releaseFrames > kMaxHysteresisFrames) return false;
    return true;
}

size_t QCamera3Occlusion::samples() const {
    if (!mImpl) return 0;
    pthread_mutex_lock(&mImpl->lock);
    const size_t value = mImpl->lastSamples;
    pthread_mutex_unlock(&mImpl->lock);
    return value;
}

void QCamera3Occlusion::reset() {
    if (!mImpl) return;
    pthread_mutex_lock(&mImpl->lock);
    mImpl->occluded = 0;
    mImpl->latchCount = mImpl->releaseCount = 0;
    pthread_mutex_unlock(&mImpl->lock);
}

int32_t QCamera3Occlusion::analyse(const Image &source, const Options &options,
        Result &result, const std::atomic<bool> *cancelled) {
    result = {};
    result.frameNumber = options.frameNumber;
    result.path = PATH_NATIVE;
    result.coreStatus = stockCoreAvailable() ? CORE_PRESENT_NOT_CALLED : CORE_ABSENT;
    if (!mImpl) return -ENOMEM;
    if (!validOptions(options)) return -EINVAL;
    if (!validImage(source) || !supports(source.width, source.height)) {
        result.reason = REASON_INVALID;
        return -EINVAL;
    }
    if (isCancelled(cancelled)) {
        result.reason = REASON_CANCELLED;
        return -ECANCELED;
    }

    // Fixed 32 x 32 block grid; up to 16 samples per block, so the sampled
    // count and the work are bounded for every accepted geometry.
    const uint32_t blockWidth = std::max(1u, source.width / kBlockGrid);
    const uint32_t blockHeight = std::max(1u, source.height / kBlockGrid);
    const uint32_t stepX = std::max(1u, blockWidth / kBlockSamples);
    const uint32_t stepY = std::max(1u, blockHeight / kBlockSamples);
    double blockMean[kBlockGrid * kBlockGrid] = {};
    double lumaSum = 0.0, lumaSquareSum = 0.0, chromaSum = 0.0;
    uint64_t sampleCount = 0, chromaCount = 0;
    bool cancelledMidway = false;
    for (uint32_t by = 0; by < kBlockGrid && !cancelledMidway; ++by) {
        for (uint32_t bx = 0; bx < kBlockGrid; ++bx) {
            if (((by * kBlockGrid + bx) % kCancelBlocks) == 0 && isCancelled(cancelled)) {
                cancelledMidway = true;
                break;
            }
            double sum = 0.0, chroma = 0.0;
            uint32_t count = 0;
            const uint32_t y0 = by * blockHeight;
            const uint32_t x0 = bx * blockWidth;
            for (uint32_t sy = 0; sy < kBlockSamples; ++sy) {
                const uint32_t y = y0 + sy * stepY;
                if (y >= source.height) break;
                const uint8_t *row = source.data + source.yOffset + size_t(y) * source.yStride;
                const uint8_t *uvRow = source.data + source.uvOffset +
                        size_t(y / 2) * source.uvStride;
                for (uint32_t sx = 0; sx < kBlockSamples; ++sx) {
                    const uint32_t x = x0 + sx * stepX;
                    if (x >= source.width) break;
                    const uint8_t luma = row[x];
                    sum += luma;
                    lumaSum += luma;
                    lumaSquareSum += double(luma) * double(luma);
                    ++count;
                    ++sampleCount;
                    const uint32_t cx = x / 2;
                    if (cx < source.width / 2) {
                        const uint8_t b0 = uvRow[size_t(cx) * 2];
                        const uint8_t b1 = uvRow[size_t(cx) * 2 + 1];
                        // Chroma byte order (NV12 vs NV21) is irrelevant to the
                        // magnitude statistic: both bytes are colour channels.
                        chroma += (b0 > 128 ? b0 - 128 : 128 - b0) +
                                (b1 > 128 ? b1 - 128 : 128 - b1);
                        chromaCount += 2;
                    }
                }
            }
            blockMean[by * kBlockGrid + bx] = count ? sum / count : 0.0;
            chromaSum += chroma;
        }
    }
    if (cancelledMidway) {
        result.reason = REASON_CANCELLED;
        return -ECANCELED;
    }
    if (!sampleCount || sampleCount > kMaxSamples) {
        result.reason = REASON_INVALID;
        return -EINVAL;
    }
    const double meanLuma = lumaSum / double(sampleCount);
    const double pixelVariance = lumaSquareSum / double(sampleCount) - meanLuma * meanLuma;
    const double pixelStd = pixelVariance > 0.0 ? std::sqrt(pixelVariance) : 0.0;
    double blockMeanSum = 0.0;
    for (double value : blockMean) blockMeanSum += value;
    const double blockMeanAverage = blockMeanSum / double(kBlockGrid * kBlockGrid);
    double blockVariance = 0.0;
    for (double value : blockMean) {
        const double delta = value - blockMeanAverage;
        blockVariance += delta * delta;
    }
    blockVariance /= double(kBlockGrid * kBlockGrid);
    const double chromaCast = chromaCount ? chromaSum / double(chromaCount) : 0.0;

    // Evidence. flatScore is the low-frequency structure absence, brightScore
    // removes the "dark room" ambiguity, castScore adds the colour cast a
    // covered lens usually shows and activityScore is the high-frequency
    // texture that separates a real scene from a blocked lens. A dark frame
    // that still carries noise is plausible and can never reach the default
    // latch threshold.
    const double flatScore = (double(kFlatVarMilli) / 1000.0 - blockVariance) /
            (double(kFlatVarMilli) / 1000.0);
    const double brightScore = meanLuma >= double(kDarkLuma) ? 1.0 :
            meanLuma / double(kDarkLuma);
    const double castScore = (chromaCast - double(kCastMinMilli) / 1000.0) /
            (double(kCastScaleMilli) / 1000.0);
    const double activityScore = (pixelStd - double(kActivityFloorMilli) / 1000.0) /
            (double(kActivityScaleMilli) / 1000.0);
    const bool darkPlausible = meanLuma < double(kDarkLuma) &&
            pixelStd >= double(kDarkNoiseStdMilli) / 1000.0;
    const int32_t flatMilli = clampScore(flatScore);
    const int32_t brightMilli = clampScore(brightScore);
    const int32_t castMilli = clampScore(castScore);
    const int32_t activityMilli = clampScore(activityScore);
    // Flatness only counts as evidence when the frame carries no per-pixel
    // texture: a fine checkerboard has flat block means but is not a blocked
    // lens. Weights sum to 1000/1000: structure 80%, brightness 15%, cast 5%.
    const int32_t structureMilli = int32_t((int64_t(flatMilli) *
            (1000 - activityMilli)) / 1000);
    int32_t confidence = (structureMilli * 80 + brightMilli * 15 + castMilli * 5) / 1000;
    if (confidence < 0) confidence = 0;
    if (confidence > 100) confidence = 100;
    if (darkPlausible && confidence > kDarkConfidenceCap) confidence = kDarkConfidenceCap;

    pthread_mutex_lock(&mImpl->lock);
    if (confidence >= options.threshold) {
        mImpl->releaseCount = 0;
        if (mImpl->latchCount < options.latchFrames) ++mImpl->latchCount;
        if (mImpl->latchCount >= options.latchFrames) mImpl->occluded = 1;
    } else if (confidence <= options.releaseThreshold) {
        mImpl->latchCount = 0;
        if (mImpl->releaseCount < options.releaseFrames) ++mImpl->releaseCount;
        if (mImpl->releaseCount >= options.releaseFrames) mImpl->occluded = 0;
    } else {
        // Between the thresholds the counters hold: documented hysteresis band.
    }
    result.occluded = mImpl->occluded;
    result.latchCount = mImpl->latchCount;
    result.releaseCount = mImpl->releaseCount;
    mImpl->lastSamples = size_t(sampleCount);
    pthread_mutex_unlock(&mImpl->lock);

    result.status = 0;
    result.applied = 1;
    result.confidence = confidence;
    result.meanLuma = int32_t(std::lround(meanLuma));
    result.lowFrequencyVarMilli = int32_t(std::lround(blockVariance * 1000.0));
    result.chromaCastMilli = int32_t(std::lround(chromaCast * 1000.0));
    result.pixelStdMilli = int32_t(std::lround(pixelStd * 1000.0));
    result.activityMilli = activityMilli;
    result.darkPlausible = darkPlausible ? 1 : 0;
    if (result.occluded) {
        result.reason = meanLuma >= double(kDarkLuma) ? REASON_OCCLUDED_FLAT_BRIGHT
                                                     : REASON_OCCLUDED_FLAT_DARK;
    } else if (darkPlausible) {
        result.reason = REASON_DARK_SCENE_PLAUSIBLE;
    } else if (structureMilli < 500) {
        result.reason = REASON_STRUCTURE;
    } else {
        result.reason = REASON_LOW_CONFIDENCE;
    }
    result.detail[0] = result.occluded;
    result.detail[1] = result.confidence;
    result.detail[2] = result.reason;
    result.detail[3] = result.path;
    return 0;
}

} // namespace qcamera
