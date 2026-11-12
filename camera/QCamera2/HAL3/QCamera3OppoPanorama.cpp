/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * OSS reimplementation (prototype) of the stock DocPanorama/LFS pipeline.
 * Pure C++ (host testable); no Android, no stock module, no vendor blob.
 *
 * Approximation boundary (task-139 §5): gyro prior + image shift search,
 * linear feather blending, overlap exposure matching, bbox crop and bilinear
 * resize. LFS internals (aligner/blending/joiner/cc/cropping) are not
 * recoverable, so output is behaviourally similar but NOT pixel exact.
 */
#include "QCamera3OppoPanorama.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>
#include <utility>

namespace qcamera {

namespace {

bool mulOverflow(size_t a, size_t b, size_t *out) {
    if (a != 0 && b > SIZE_MAX / a)
        return true;
    *out = a * b;
    return false;
}

size_t chromaRows(uint32_t height) { return height / 2; }

int32_t clampInt(int32_t v, int32_t lo, int32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

float clampFloat(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

int32_t roundToInt(float v) {
    return static_cast<int32_t>(v >= 0.0f ? v + 0.5f : v - 0.5f);
}

// Rotate NV21 (w x h) by 180/90/270 into dst (sized for swapped dims).
void rotateNv21(const uint8_t *src, uint32_t w, uint32_t h, int32_t rotation,
        uint8_t *dst, uint32_t *outW, uint32_t *outH) {
    const size_t srcYSize = static_cast<size_t>(w) * h;
    const uint8_t *srcUv = src + srcYSize;
    if (rotation == 180) {
        for (uint32_t y = 0; y < h; y++) {
            for (uint32_t x = 0; x < w; x++)
                dst[static_cast<size_t>(y) * w + x] =
                        src[static_cast<size_t>(h - 1 - y) * w + (w - 1 - x)];
        }
        const size_t uvSize = static_cast<size_t>(w) * (h / 2);
        for (size_t i = 0; i + 1 < uvSize; i += 2) {
            const size_t j = uvSize - 2 - i;
            dst[srcYSize + i] = srcUv[j];
            dst[srcYSize + i + 1] = srcUv[j + 1];
        }
        *outW = w;
        *outH = h;
        return;
    }
    const uint32_t dw = h;
    const uint32_t dh = w;
    for (uint32_t y = 0; y < dh; y++) {
        for (uint32_t x = 0; x < dw; x++) {
            uint32_t sx;
            uint32_t sy;
            if (rotation == 90) { // clockwise
                sx = y;
                sy = h - 1 - x;
            } else { // 270
                sx = w - 1 - y;
                sy = x;
            }
            dst[static_cast<size_t>(y) * dw + x] =
                    src[static_cast<size_t>(sy) * w + sx];
        }
    }
    const uint32_t sw = w / 2;
    const uint32_t sh = h / 2;
    const uint32_t duvW = dw / 2;
    const size_t dstYSize = static_cast<size_t>(dw) * dh;
    for (uint32_t y = 0; y < dh / 2; y++) {
        for (uint32_t x = 0; x < duvW; x++) {
            uint32_t sx;
            uint32_t sy;
            if (rotation == 90) {
                sx = y;
                sy = sh - 1 - x;
            } else {
                sx = sw - 1 - y;
                sy = x;
            }
            const size_t si = (static_cast<size_t>(sy) * sw + sx) * 2;
            const size_t di = dstYSize + (static_cast<size_t>(y) * duvW + x) * 2;
            dst[di] = srcUv[si];
            dst[di + 1] = srcUv[si + 1];
        }
    }
    *outW = dw;
    *outH = dh;
}

void bilinearResizeY(const uint8_t *src, uint32_t sw, uint32_t sh, uint32_t sStride,
        uint8_t *dst, uint32_t dw, uint32_t dh) {
    for (uint32_t y = 0; y < dh; y++) {
        const float fy = (static_cast<float>(y) + 0.5f) * sh / dh - 0.5f;
        const int32_t y0 = clampInt(static_cast<int32_t>(std::floor(fy)), 0,
                static_cast<int32_t>(sh) - 1);
        const int32_t y1 = std::min<int32_t>(y0 + 1, static_cast<int32_t>(sh) - 1);
        const float wy = fy - y0;
        const uint8_t *row0 = src + static_cast<size_t>(y0) * sStride;
        const uint8_t *row1 = src + static_cast<size_t>(y1) * sStride;
        for (uint32_t x = 0; x < dw; x++) {
            const float fx = (static_cast<float>(x) + 0.5f) * sw / dw - 0.5f;
            const int32_t x0 = clampInt(static_cast<int32_t>(std::floor(fx)), 0,
                    static_cast<int32_t>(sw) - 1);
            const int32_t x1 = std::min<int32_t>(x0 + 1, static_cast<int32_t>(sw) - 1);
            const float wx = fx - x0;
            const float v00 = row0[x0];
            const float v01 = row0[x1];
            const float v10 = row1[x0];
            const float v11 = row1[x1];
            const float v = (v00 * (1 - wx) + v01 * wx) * (1 - wy) +
                    (v10 * (1 - wx) + v11 * wx) * wy;
            dst[static_cast<size_t>(y) * dw + x] =
                    static_cast<uint8_t>(clampInt(roundToInt(v), 0, 255));
        }
    }
}

void resizeUvNearest(const uint8_t *src, uint32_t sw, uint32_t sh, uint32_t sStride,
        uint8_t *dst, uint32_t dw, uint32_t dh) {
    const uint32_t suw = sw / 2;
    const uint32_t duw = dw / 2;
    const uint32_t dh2 = dh / 2;
    const uint32_t sh2 = sh / 2;
    for (uint32_t y = 0; y < dh2; y++) {
        const uint32_t sy = std::min<uint32_t>(y * sh2 / dh2, sh2 - 1);
        for (uint32_t x = 0; x < duw; x++) {
            const uint32_t sx = std::min<uint32_t>(x * suw / duw, suw - 1);
            const size_t si = static_cast<size_t>(sy) * sStride + sx * 2;
            const size_t di = static_cast<size_t>(y) * dw + x * 2;
            dst[di] = src[si];
            dst[di + 1] = src[si + 1];
        }
    }
}

} // namespace

struct QCamera3OppoPanorama::Impl {
    uint8_t *canvasY = nullptr;
    uint8_t *canvasUv = nullptr;
    uint8_t *weight = nullptr;
    uint8_t *prevY = nullptr;
    uint32_t prevW = 0;
    uint32_t prevH = 0;
    bool hasPrev = false;
    int32_t bboxMinX = 0;
    int32_t bboxMinY = 0;
    int32_t bboxMaxX = 0;
    int32_t bboxMaxY = 0;
    bool hasBbox = false;
    float pendingGyroX = 0.0f;
    float pendingGyroY = 0.0f;
    bool pendingGyroValid = false;
    size_t allocatedBytes = 0;
    uint8_t *stitchTemp = nullptr;
    size_t stitchTempSize = 0;
    uint8_t *rotateTemp = nullptr;
    size_t rotateTempSize = 0;

    void freeAll() {
        delete[] canvasY;
        delete[] canvasUv;
        delete[] weight;
        delete[] prevY;
        delete[] stitchTemp;
        delete[] rotateTemp;
        canvasY = canvasUv = weight = prevY = stitchTemp = rotateTemp = nullptr;
        stitchTempSize = rotateTempSize = 0;
        allocatedBytes = 0;
        hasPrev = false;
        hasBbox = false;
        pendingGyroValid = false;
    }
};

QCamera3OppoPanorama::QCamera3OppoPanorama() : impl_(new Impl()), state_(State::Idle) {
    stats_.lastStatus = OK;
}

QCamera3OppoPanorama::~QCamera3OppoPanorama() {
    deinit();
    delete impl_;
    impl_ = nullptr;
}

size_t QCamera3OppoPanorama::nv21Size(uint32_t width, uint32_t height,
        uint32_t yStride, uint32_t uvStride) {
    if (width == 0 || height == 0 || (width & 1) || (height & 1))
        return 0;
    const size_t ys = yStride != 0 ? yStride : width;
    const size_t uvs = uvStride != 0 ? uvStride : width;
    size_t total = 0;
    size_t uv = 0;
    if (mulOverflow(ys, height, &total) || mulOverflow(uvs, chromaRows(height), &uv) ||
            total > SIZE_MAX - uv)
        return 0;
    return total + uv;
}

bool QCamera3OppoPanorama::validFrame(const Frame &frame) {
    if (frame.data == nullptr || frame.width == 0 || frame.height == 0)
        return false;
    if ((frame.width & 1) || (frame.height & 1))
        return false;
    if (frame.yStride < frame.width || frame.uvStride < frame.width)
        return false;
    const size_t need = nv21Size(frame.width, frame.height, frame.yStride,
            frame.uvStride);
    return need != 0 && frame.length >= need;
}

bool QCamera3OppoPanorama::stockAspectSize(uint32_t width, uint32_t height,
        uint32_t *outWidth, uint32_t *outHeight) {
    if (width == 0 || height == 0 || outWidth == nullptr || outHeight == nullptr)
        return false;
    // Table read from the stock ctor call site (task-139 §1):
    // 4:3 -> 320x240; 16:9 -> 640x360; 2:1 -> 640x320; 19:9 -> 646x306;
    // 13:6 -> 650x300.
    if (3 * width == 4 * height) {
        *outWidth = 320;
        *outHeight = 240;
        return true;
    }
    if (9 * width == 16 * height) {
        *outWidth = 640;
        *outHeight = 360;
        return true;
    }
    if (9 * width == 18 * height) {
        *outWidth = 640;
        *outHeight = 320;
        return true;
    }
    if (9 * width == 19 * height) {
        *outWidth = 646;
        *outHeight = 306;
        return true;
    }
    if (6 * width == 13 * height) {
        *outWidth = 650;
        *outHeight = 300;
        return true;
    }
    return false;
}

int32_t QCamera3OppoPanorama::stockDirectionCode(int32_t directionDeg) {
    switch (directionDeg) {
    case 0:
        return 0x12;
    case 90:
        return 0x11;
    case 180:
        return 0x22;
    case 270:
        return 0x21;
    default:
        return -1;
    }
}

int32_t QCamera3OppoPanorama::fail(int32_t status, const char *where) {
    (void)where;
    stats_.lastStatus = status;
    if (status == E_CANCELED)
        state_ = State::Canceled;
    return status;
}

int32_t QCamera3OppoPanorama::init(const Config &config) {
    deinit();
    stats_ = Stats();
    stats_.lastStatus = OK;
    offset_ = Offset();
    config_ = config;

    const auto even = [](uint32_t v) { return (v & 1u) == 0u; };
    if (config.previewWidth == 0 || config.previewHeight == 0 ||
            !even(config.previewWidth) || !even(config.previewHeight))
        return fail(E_PARAM, "preview size");
    const bool okOrient = config.orientationDeg == 0 || config.orientationDeg == 90 ||
            config.orientationDeg == 180 || config.orientationDeg == 270;
    if (!okOrient)
        return fail(E_UNSUPPORTED, "orientation");
    if (config.directionDeg != DIR_AUTO && stockDirectionCode(config.directionDeg) < 0)
        return fail(E_UNSUPPORTED, "direction");
    if (config.maxFrames == 0 || config.minFrames == 0 || config.maxFrames < config.minFrames)
        return fail(E_PARAM, "frame limits");
    if (!std::isfinite(config.pxPerDeg) || config.pxPerDeg < 0.0f)
        return fail(E_PARAM, "gyro scale");

    canvasW_ = config.canvasWidth;
    canvasH_ = config.canvasHeight;
    if (canvasW_ == 0 && canvasH_ == 0) {
        if (config.previewWidth > UINT32_MAX / 3)
            return fail(E_NOMEM, "canvas size");
        canvasW_ = config.previewWidth * 3;
        canvasH_ = config.previewHeight;
    } else if (canvasW_ == 0 || canvasH_ == 0) {
        return fail(E_PARAM, "canvas size");
    }
    if (!even(canvasW_) || !even(canvasH_) || canvasW_ < config.previewWidth ||
            canvasH_ < config.previewHeight)
        return fail(E_PARAM, "canvas size");
    if (config.centerCanvas) {
        offset_.x = int32_t((canvasW_ - config.previewWidth) / 2) & ~1;
        offset_.y = int32_t((canvasH_ - config.previewHeight) / 2) & ~1;
    } else {
        if (config.directionDeg == 180) offset_.x = canvasW_ - config.previewWidth;
        if (config.directionDeg == 270) offset_.y = canvasH_ - config.previewHeight;
    }

    outW_ = config.outputWidth;
    outH_ = config.outputHeight;
    if (outW_ == 0 || outH_ == 0) {
        if (!stockAspectSize(config.previewWidth, config.previewHeight, &outW_, &outH_)) {
            outW_ = canvasW_;
            outH_ = canvasH_;
        }
    }
    if (!even(outW_) || !even(outH_) || outW_ == 0 || outH_ == 0)
        return fail(E_PARAM, "output size");

    size_t canvasYSize = 0;
    size_t canvasUvSize = 0;
    size_t prevSize = 0;
    size_t stitchSize = 0;
    if (mulOverflow(canvasW_, canvasH_, &canvasYSize) ||
            mulOverflow(canvasW_, chromaRows(canvasH_), &canvasUvSize) ||
            mulOverflow(config.previewWidth, config.previewHeight, &prevSize) ||
            mulOverflow(outW_, outH_, &stitchSize))
        return fail(E_NOMEM, "canvas overflow");
    size_t total = canvasYSize;
    const size_t parts[] = {canvasUvSize, canvasYSize, prevSize, stitchSize};
    for (size_t part : parts) {
        if (total > SIZE_MAX - part)
            return fail(E_NOMEM, "canvas overflow");
        total += part;
    }
    if (total > config.maxCanvasBytes)
        return fail(E_NOMEM, "memory budget");

    impl_->canvasY = new (std::nothrow) uint8_t[canvasYSize]();
    impl_->canvasUv = new (std::nothrow) uint8_t[canvasUvSize]();
    impl_->weight = new (std::nothrow) uint8_t[canvasYSize]();
    impl_->prevY = new (std::nothrow) uint8_t[prevSize]();
    impl_->stitchTemp = new (std::nothrow) uint8_t[stitchSize]();
    impl_->stitchTempSize = stitchSize;
    if (impl_->canvasY == nullptr || impl_->canvasUv == nullptr ||
            impl_->weight == nullptr || impl_->prevY == nullptr ||
            impl_->stitchTemp == nullptr) {
        impl_->freeAll();
        return fail(E_NOMEM, "allocate");
    }
    impl_->allocatedBytes = total;
    stats_.peakBytes = total;
    state_ = State::Inited;
    return OK;
}

int32_t QCamera3OppoPanorama::reinit(const Config &config) {
    return init(config);
}

void QCamera3OppoPanorama::deinit() {
    if (impl_ != nullptr)
        impl_->freeAll();
    state_ = State::Idle;
    offset_ = Offset();
    canvasW_ = canvasH_ = outW_ = outH_ = 0;
}

void QCamera3OppoPanorama::cancel() {
    if (state_ == State::Idle)
        return;
    state_ = State::Canceled;
    stats_.lastStatus = E_CANCELED;
}

int32_t QCamera3OppoPanorama::addGyro(const GyroSample &gyro) {
    if (state_ != State::Inited && state_ != State::Capturing)
        return fail(E_STATE, "addGyro");
    if (gyro.valid && (!std::isfinite(gyro.angleXDeg) || !std::isfinite(gyro.angleYDeg)))
        return fail(E_PARAM, "gyro sample");
    impl_->pendingGyroX = gyro.angleXDeg;
    impl_->pendingGyroY = gyro.angleYDeg;
    impl_->pendingGyroValid = gyro.valid;
    return OK;
}

void QCamera3OppoPanorama::applySign(int sign, int32_t *shiftX, int32_t *shiftY) {
    if (sign > 0) {
        if (*shiftX < 0) *shiftX = 0;
        if (*shiftY < 0) *shiftY = 0;
    } else if (sign < 0) {
        if (*shiftX > 0) *shiftX = 0;
        if (*shiftY > 0) *shiftY = 0;
    }
}

void QCamera3OppoPanorama::estimateShift(const Frame &frame, bool gyroValid,
        int32_t *shiftX, int32_t *shiftY) {
    *shiftX = 0;
    *shiftY = 0;
    const uint32_t fw = frame.width;
    const uint32_t fh = frame.height;
    const uint8_t *srcY = frame.data;

    int axis = config_.verticalSweep ? 1 : 0; // 0 = x, 1 = y
    int sign = 0; // 0 = both, +1 / -1 constrained
    if (config_.directionDeg == 90 || config_.directionDeg == 270) {
        axis = 1;
        sign = (config_.directionDeg == 90) ? 1 : -1;
    } else if (config_.directionDeg == 0) {
        sign = 1;
    } else if (config_.directionDeg == 180) {
        sign = -1;
    }

    const int32_t prior = (gyroValid && config_.pxPerDeg > 0.0f)
            ? roundToInt((axis == 0 ? impl_->pendingGyroX : impl_->pendingGyroY) *
                    config_.pxPerDeg)
            : 0;
    const int32_t radius = config_.searchRadiusPx > 0 ? config_.searchRadiusPx : 0;

    if (!impl_->hasPrev) {
        // The first frame defines the reference without an inter-frame shift.
        *shiftX = 0;
        *shiftY = 0;
        return;
    }

    const int32_t maxShift = (axis == 0) ? (static_cast<int32_t>(fw) - 8)
                                         : (static_cast<int32_t>(fh) - 8);
    int32_t lo = -radius;
    int32_t hi = radius;
    if (gyroValid && config_.pxPerDeg > 0.0f) {
        lo = prior - radius / 2;
        hi = prior + radius / 2;
    }
    if (sign > 0)
        lo = std::max<int32_t>(lo, 0);
    if (sign < 0)
        hi = std::min<int32_t>(hi, 0);
    lo = clampInt(lo, -maxShift, maxShift);
    hi = clampInt(hi, -maxShift, maxShift);
    if (lo > hi)
        std::swap(lo, hi);

    constexpr int32_t kStep = 2;
    int32_t best = 0;
    uint64_t bestSad = UINT64_MAX;
    for (int32_t s = lo; s <= hi; s += kStep) {
        uint64_t sad = 0;
        uint32_t count = 0;
        if (axis == 0) {
            const int32_t overlapW = static_cast<int32_t>(fw) - std::abs(s);
            if (overlapW <= 4)
                continue;
            for (uint32_t y = 2; y < fh; y += 4) {
                for (uint32_t x = 2; x < static_cast<uint32_t>(overlapW); x += 4) {
                    const uint32_t px = (s >= 0) ? x + static_cast<uint32_t>(s) : x;
                    const uint32_t cx = (s >= 0) ? x : x + static_cast<uint32_t>(-s);
                    const int dif = static_cast<int>(
                            srcY[static_cast<size_t>(y) * frame.yStride + cx]) -
                            static_cast<int>(impl_->prevY[static_cast<size_t>(y) * fw + px]);
                    sad += static_cast<uint64_t>(dif < 0 ? -dif : dif);
                    count++;
                }
            }
        } else {
            const int32_t overlapH = static_cast<int32_t>(fh) - std::abs(s);
            if (overlapH <= 4)
                continue;
            for (uint32_t y = 2; y < static_cast<uint32_t>(overlapH); y += 4) {
                for (uint32_t x = 2; x < fw; x += 4) {
                    const uint32_t py = (s >= 0) ? y + static_cast<uint32_t>(s) : y;
                    const uint32_t cy = (s >= 0) ? y : y + static_cast<uint32_t>(-s);
                    const int dif = static_cast<int>(
                            srcY[static_cast<size_t>(cy) * frame.yStride + x]) -
                            static_cast<int>(impl_->prevY[static_cast<size_t>(py) * fw + x]);
                    sad += static_cast<uint64_t>(dif < 0 ? -dif : dif);
                    count++;
                }
            }
        }
        if (count == 0)
            continue;
        if (sad < bestSad ||
                (sad == bestSad && std::abs(s - prior) < std::abs(best - prior))) {
            bestSad = sad;
            best = s;
        }
    }
    if (bestSad == UINT64_MAX)
        best = prior;
    if (axis == 0)
        *shiftX = best;
    else
        *shiftY = best;
}

int32_t QCamera3OppoPanorama::addFrame(const Frame &frame, const GyroSample *gyro) {
    if (state_ == State::Canceled)
        return fail(E_CANCELED, "addFrame");
    if (state_ != State::Inited && state_ != State::Capturing)
        return fail(E_STATE, "addFrame");
    if (!validFrame(frame))
        return fail(E_PARAM, "frame");
    if (frame.width != config_.previewWidth || frame.height != config_.previewHeight)
        return fail(E_PARAM, "frame size");
    if (stats_.frames >= config_.maxFrames)
        return fail(E_LIMIT, "maxFrames");

    if (gyro != nullptr) {
        const int32_t status = addGyro(*gyro);
        if (status != OK) return status;
    }

    int32_t shiftX = 0;
    int32_t shiftY = 0;
    estimateShift(frame, impl_->pendingGyroValid, &shiftX, &shiftY);
    impl_->pendingGyroValid = false;
    stats_.shiftPx = (shiftX != 0) ? shiftX : shiftY;
    const int32_t newX = offset_.x + shiftX;
    const int32_t newY = offset_.y + shiftY;
    if (newX < 0 || newY < 0 ||
            static_cast<uint32_t>(newX) + frame.width > canvasW_ ||
            static_cast<uint32_t>(newY) + frame.height > canvasH_) {
        stats_.rejected++;
        return fail(E_OVERFLOW, "frame out of canvas");
    }

    const uint8_t *srcY = frame.data;
    const uint8_t *srcUv = frame.data + static_cast<size_t>(frame.yStride) * frame.height;
    const uint32_t fw = frame.width;
    const uint32_t fh = frame.height;

    // 1) Exposure gain from the overlap with already written canvas pixels.
    float gain = 1.0f;
    if (impl_->hasPrev) {
        uint64_t sumOld = 0;
        uint64_t sumNew = 0;
        uint32_t count = 0;
        for (uint32_t y = 1; y < fh; y += 4) {
            const uint32_t cy = static_cast<uint32_t>(newY) + y;
            for (uint32_t x = 1; x < fw; x += 4) {
                const uint32_t cx = static_cast<uint32_t>(newX) + x;
                const size_t ci = static_cast<size_t>(cy) * canvasW_ + cx;
                if (impl_->weight[ci] == 0)
                    continue;
                sumOld += impl_->canvasY[ci];
                sumNew += srcY[static_cast<size_t>(y) * frame.yStride + x];
                count++;
            }
        }
        if (count > 32 && sumNew > 0) {
            gain = clampFloat(static_cast<float>(sumOld) / static_cast<float>(sumNew),
                    1.0f / config_.maxExposureGain, config_.maxExposureGain);
        }
    }

    // 2) Blend the frame into the canvas with feather weights.
    const int32_t feather = static_cast<int32_t>(config_.featherPx);
    for (uint32_t y = 0; y < fh; y++) {
        const uint32_t cy = static_cast<uint32_t>(newY) + y;
        for (uint32_t x = 0; x < fw; x++) {
            const uint32_t cx = static_cast<uint32_t>(newX) + x;
            const size_t ci = static_cast<size_t>(cy) * canvasW_ + cx;
            uint32_t alpha = 255;
            if (feather > 0) {
                const int32_t dx = std::min<int32_t>(static_cast<int32_t>(x),
                        static_cast<int32_t>(fw - 1 - x));
                const int32_t dy = std::min<int32_t>(static_cast<int32_t>(y),
                        static_cast<int32_t>(fh - 1 - y));
                const int32_t d = std::min(dx, dy);
                if (d < feather)
                    alpha = static_cast<uint32_t>(std::max<int32_t>(1,
                            d * 255 / feather));
            }
            const uint32_t oldW = impl_->weight[ci];
            const uint32_t oldYv = impl_->canvasY[ci];
            const float newYf = clampFloat(srcY[static_cast<size_t>(y) * frame.yStride + x] *
                    gain, 0.0f, 255.0f);
            const uint32_t newYv = static_cast<uint32_t>(roundToInt(newYf));
            const uint32_t wsum = oldW + alpha;
            uint32_t blended;
            if (oldW == 0)
                blended = newYv;
            else
                blended = (oldYv * oldW + newYv * alpha) / wsum;
            impl_->canvasY[ci] = static_cast<uint8_t>(std::min<uint32_t>(blended, 255));
            impl_->weight[ci] = static_cast<uint8_t>(std::min<uint32_t>(wsum, 255));

            if (((x & 1u) == 0u) && ((y & 1u) == 0u) && ((cx & 1u) == 0u) &&
                    ((cy & 1u) == 0u)) {
                const size_t cUv = static_cast<size_t>(cy / 2) * canvasW_ + cx;
                const size_t sUv = static_cast<size_t>(y / 2) * frame.uvStride + x;
                const size_t uvLimit = static_cast<size_t>(canvasW_) * chromaRows(canvasH_);
                if (cUv + 1 < uvLimit &&
                        sUv + 1 < static_cast<size_t>(frame.uvStride) * chromaRows(fh)) {
                    const bool uvEmpty = (oldW == 0) ||
                            (impl_->canvasUv[cUv] == 0 && impl_->canvasUv[cUv + 1] == 0);
                    if (uvEmpty) {
                        impl_->canvasUv[cUv] = srcUv[sUv];
                        impl_->canvasUv[cUv + 1] = srcUv[sUv + 1];
                    } else if (wsum != 0) {
                        impl_->canvasUv[cUv] = static_cast<uint8_t>(
                                (impl_->canvasUv[cUv] * oldW + srcUv[sUv] * alpha) / wsum);
                        impl_->canvasUv[cUv + 1] = static_cast<uint8_t>(
                                (impl_->canvasUv[cUv + 1] * oldW + srcUv[sUv + 1] * alpha) / wsum);
                    }
                }
            }
        }
    }

    // 3) Update bbox and remember this frame for the next shift estimate.
    if (!impl_->hasBbox) {
        impl_->bboxMinX = newX;
        impl_->bboxMinY = newY;
        impl_->bboxMaxX = newX + static_cast<int32_t>(fw);
        impl_->bboxMaxY = newY + static_cast<int32_t>(fh);
        impl_->hasBbox = true;
    } else {
        impl_->bboxMinX = std::min<int32_t>(impl_->bboxMinX, newX);
        impl_->bboxMinY = std::min<int32_t>(impl_->bboxMinY, newY);
        impl_->bboxMaxX = std::max<int32_t>(impl_->bboxMaxX, newX + static_cast<int32_t>(fw));
        impl_->bboxMaxY = std::max<int32_t>(impl_->bboxMaxY, newY + static_cast<int32_t>(fh));
    }
    for (uint32_t y = 0; y < fh; y++)
        std::memcpy(impl_->prevY + static_cast<size_t>(y) * fw,
                srcY + static_cast<size_t>(y) * frame.yStride, fw);
    impl_->prevW = fw;
    impl_->prevH = fh;
    impl_->hasPrev = true;
    offset_.x = newX;
    offset_.y = newY;
    stats_.frames++;
    stats_.lastGain = gain;
    stats_.lastStatus = OK;
    state_ = State::Capturing;
    return OK;
}

int32_t QCamera3OppoPanorama::finetune() {
    if (state_ == State::Canceled)
        return fail(E_CANCELED, "finetune");
    if (state_ != State::Capturing && state_ != State::Inited)
        return fail(E_STATE, "finetune");
    if (stats_.frames < config_.minFrames)
        return fail(E_NOFRAME, "finetune");
    if (config_.crop && config_.preserveCropSize) {
        outW_ = uint32_t((impl_->bboxMaxX & ~1) - (impl_->bboxMinX & ~1));
        outH_ = uint32_t((impl_->bboxMaxY & ~1) - (impl_->bboxMinY & ~1));
    }
    state_ = State::Finetuned;
    stats_.lastStatus = OK;
    return OK;
}

size_t QCamera3OppoPanorama::outputSize() const {
    if (outW_ == 0 || outH_ == 0)
        return 0;
    const bool rotate = (config_.orientationDeg == 90 || config_.orientationDeg == 270);
    const uint32_t w = rotate ? outH_ : outW_;
    const uint32_t h = rotate ? outW_ : outH_;
    return nv21Size(w, h, w, w);
}

int32_t QCamera3OppoPanorama::stitch(Image *out) {
    if (state_ == State::Canceled)
        return fail(E_CANCELED, "stitch");
    if (out == nullptr || out->data == nullptr)
        return fail(E_PARAM, "stitch output");
    if (state_ != State::Capturing && state_ != State::Finetuned &&
            state_ != State::Stitched)
        return fail(E_STATE, "stitch");
    if (stats_.frames < config_.minFrames)
        return fail(E_NOFRAME, "stitch");

    int32_t minX;
    int32_t minY;
    int32_t maxX;
    int32_t maxY;
    if (config_.crop) {
        minX = impl_->bboxMinX;
        minY = impl_->bboxMinY;
        maxX = impl_->bboxMaxX;
        maxY = impl_->bboxMaxY;
    } else {
        minX = 0;
        minY = 0;
        maxX = static_cast<int32_t>(canvasW_);
        maxY = static_cast<int32_t>(canvasH_);
    }
    minX = clampInt(minX, 0, static_cast<int32_t>(canvasW_) - 2) & ~1;
    minY = clampInt(minY, 0, static_cast<int32_t>(canvasH_) - 2) & ~1;
    maxX = clampInt(maxX, minX + 2, static_cast<int32_t>(canvasW_)) & ~1;
    maxY = clampInt(maxY, minY + 2, static_cast<int32_t>(canvasH_)) & ~1;
    const uint32_t cropW = static_cast<uint32_t>(maxX - minX);
    const uint32_t cropH = static_cast<uint32_t>(maxY - minY);
    if (cropW < 2 || cropH < 2)
        return fail(E_NOFRAME, "empty canvas");
    if (config_.crop && config_.preserveCropSize) {
        outW_ = cropW;
        outH_ = cropH;
    }

    const size_t baseYSize = static_cast<size_t>(cropW) * cropH;
    const size_t baseSize = baseYSize + baseYSize / 2;
    const size_t outSize = nv21Size(outW_, outH_, outW_, outW_);
    const size_t maxNeeded = std::max(baseSize, outSize);
    if (maxNeeded > impl_->stitchTempSize) {
        delete[] impl_->stitchTemp;
        impl_->stitchTemp = new (std::nothrow) uint8_t[maxNeeded]();
        impl_->stitchTempSize = maxNeeded;
        if (impl_->stitchTemp == nullptr) {
            impl_->stitchTempSize = 0;
            return fail(E_NOMEM, "stitch temp");
        }
    }
    if (outSize > out->length)
        return fail(E_PARAM, "output buffer too small");

    uint8_t *tmp = impl_->stitchTemp;
    for (uint32_t y = 0; y < cropH; y++) {
        const size_t src = static_cast<size_t>(minY + static_cast<int32_t>(y)) * canvasW_ +
                static_cast<uint32_t>(minX);
        std::memcpy(tmp + static_cast<size_t>(y) * cropW, impl_->canvasY + src, cropW);
    }
    for (uint32_t y = 0; y < cropH / 2; y++) {
        const size_t src = (static_cast<size_t>(minY / 2) + y) * canvasW_ +
                static_cast<uint32_t>(minX);
        std::memcpy(tmp + baseYSize + static_cast<size_t>(y) * cropW,
                impl_->canvasUv + src, cropW);
    }

    uint8_t *dst = out->data;
    bilinearResizeY(tmp, cropW, cropH, cropW, dst, outW_, outH_);
    resizeUvNearest(tmp + baseYSize, cropW, cropH, cropW, dst + static_cast<size_t>(outW_) * outH_,
            outW_, outH_);

    uint32_t finalW = outW_;
    uint32_t finalH = outH_;
    if (config_.orientationDeg != 0) {
        const size_t rotSize = nv21Size(outW_, outH_, outW_, outW_);
        if (rotSize > impl_->rotateTempSize) {
            delete[] impl_->rotateTemp;
            impl_->rotateTemp = new (std::nothrow) uint8_t[rotSize]();
            impl_->rotateTempSize = rotSize;
            if (impl_->rotateTemp == nullptr) {
                impl_->rotateTempSize = 0;
                return fail(E_NOMEM, "rotate temp");
            }
        }
        std::memcpy(impl_->rotateTemp, dst, rotSize);
        rotateNv21(impl_->rotateTemp, outW_, outH_, config_.orientationDeg, dst,
                &finalW, &finalH);
    }
    const size_t finalSize = nv21Size(finalW, finalH, finalW, finalW);
    if (finalSize == 0 || finalSize > out->length)
        return fail(E_PARAM, "output buffer too small after rotate");

    out->width = finalW;
    out->height = finalH;
    out->yStride = finalW;
    out->uvStride = finalW;
    out->yOffset = 0;
    out->uvOffset = static_cast<size_t>(finalW) * finalH;
    out->length = finalSize;
    out->orientationDeg = config_.orientationDeg;
    state_ = State::Stitched;
    stats_.lastStatus = OK;
    return OK;
}

} // namespace qcamera
