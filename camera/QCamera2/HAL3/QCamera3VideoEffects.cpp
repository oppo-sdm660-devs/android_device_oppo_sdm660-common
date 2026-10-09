/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3VideoEffects.h"
#include "QCamera3StockBeauty.h"
#include <algorithm>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
namespace qcamera {
namespace {
bool stopped(const std::atomic<bool> *flag) { return flag && flag->load(); }
struct Buffer {
    uint8_t *data;
    explicit Buffer(size_t size) : data(static_cast<uint8_t *>(malloc(size))) {}
    ~Buffer() { free(data); }
};
// A monotonic SDR luma curve lifts shadows and compresses highlights while
// preserving black/white endpoints. It cannot recover clipped sensor detail.
void makeCurve(uint8_t curve[256], int32_t strength) {
    for (int32_t value = 0; value < 256; ++value) {
        const int32_t studio = std::max(0, std::min(219, value - 16));
        const int32_t bend = studio < 110 ? studio * (110 - studio) / 110 :
            -(studio - 110) * (219 - studio) / 109;
        curve[value] = uint8_t(std::max(0, std::min(255, value + bend * strength / 200)));
    }
}
}
bool QCamera3VideoEffects::supports(uint32_t width, uint32_t height) {
    return width >= 32 && height >= 32 && !(width & 1) && !(height & 1) &&
        uint64_t(width) * height <= kMaxPixels;
}
bool QCamera3VideoEffects::valid(const Options &options) {
    if (options.beauty < 0 || options.beauty > 100 || options.tone < 0 ||
            options.tone > 100) return false;
    if (!(options.focalLengthPx >= 0.0f) ||
            options.focalLengthPx > QCamera3VideoEffects::kMaxFocalLengthPx) return false;
    return true;
}
int32_t QCamera3VideoEffects::process(const Image &input, bool inputNv21,
        const Image &output, bool outputNv21, const Options &options,
        const std::atomic<bool> *cancelled) {
    if (!valid(options) || !supports(input.width, input.height) ||
            !QCamera3HdrAlgo::validImage(input) || !QCamera3HdrAlgo::validImage(output) ||
            output.width != input.width || output.height != input.height) return -EINVAL;
    if (stopped(cancelled)) return -ECANCELED;
    const size_t pixels = size_t(input.width) * input.height;
    Buffer scratch{pixels * 3 / 2};
    if (!scratch.data) return -ENOMEM;
    for (uint32_t row = 0; row < input.height; ++row) {
        if (stopped(cancelled)) return -ECANCELED;
        memcpy(scratch.data + size_t(row) * input.width,
            input.data + input.yOffset + size_t(row) * input.yStride, input.width);
    }
    for (uint32_t row = 0; row < input.height / 2; ++row) {
        if (stopped(cancelled)) return -ECANCELED;
        const uint8_t *src = input.data + input.uvOffset + size_t(row) * input.uvStride;
        uint8_t *dst = scratch.data + pixels + size_t(row) * input.width;
        for (uint32_t col = 0; col < input.width; col += 2) {
            dst[col] = src[col + (inputNv21 ? 0 : 1)];
            dst[col + 1] = src[col + (inputNv21 ? 1 : 0)];
        }
    }
    Image frame = {scratch.data, pixels * 3 / 2, input.width, input.height, 0, pixels,
        input.width, input.width, input.height, input.height / 2};
    if (options.beauty) {
        QCamera3StockBeauty::Options beauty;
        beauty.soften = options.beauty;beauty.softenSharp = options.beauty / 4;
        beauty.skinBright = options.beauty / 3;
        const int32_t status = QCamera3StockBeauty::process(frame, beauty, cancelled);
        if (status) return status;
    }
    if (options.tone) {
        uint8_t curve[256];makeCurve(curve, options.tone);
        for (size_t row = 0; row < input.height; ++row) {
            if (stopped(cancelled)) return -ECANCELED;
            uint8_t *luma = scratch.data + row * input.width;
            for (uint32_t col = 0; col < input.width; ++col) luma[col] = curve[luma[col]];
        }
    }
    if (stopped(cancelled)) return -ECANCELED;
    // The commit is deliberately uncancellable: framework never sees a half
    // frame. Both input and output may reference the same heap allocation.
    for (uint32_t row = 0; row < output.height; ++row)
        memcpy(output.data + output.yOffset + size_t(row) * output.yStride,
            scratch.data + size_t(row) * output.width, output.width);
    for (uint32_t row = 0; row < output.height / 2; ++row) {
        const uint8_t *src = scratch.data + pixels + size_t(row) * output.width;
        uint8_t *dst = output.data + output.uvOffset + size_t(row) * output.uvStride;
        for (uint32_t col = 0; col < output.width; col += 2) {
            dst[col] = src[col + (outputNv21 ? 0 : 1)];
            dst[col + 1] = src[col + (outputNv21 ? 1 : 0)];
        }
    }
    return 0;
}
}
