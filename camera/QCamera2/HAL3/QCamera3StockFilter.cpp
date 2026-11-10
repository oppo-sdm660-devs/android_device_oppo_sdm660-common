/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3StockFilter.h"

#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#ifndef QCAMERA3_STOCK_FILTER_LIBRARY
#define QCAMERA3_STOCK_FILTER_LIBRARY "/vendor/lib/libfilter.so"
#endif

namespace qcamera {
namespace {
// R11/R11s libfilter Filter_Process@0x14c98 and its worker@0x14e04:
// the first word is a filter enum, unlike Filter_Process_YUV's opaque context.
using Process = int32_t (*)(int32_t, const uint8_t *, uint8_t *, int32_t, int32_t,
        int32_t, int32_t, int32_t, int32_t);
struct Api {
    void *library = nullptr;
    Process process = nullptr;
    Api() {
#if !defined(__ANDROID__) || UINTPTR_MAX == UINT32_MAX
        library = dlopen(QCAMERA3_STOCK_FILTER_LIBRARY, RTLD_NOW | RTLD_LOCAL);
        if (library) process = reinterpret_cast<Process>(dlsym(library, "Filter_Process"));
#endif
    }
    ~Api() { if (library) dlclose(library); }
};
Api &api() { static Api instance; return instance; }
struct Bytes {
    uint8_t *data;
    explicit Bytes(size_t length) : data(static_cast<uint8_t *>(malloc(length))) {}
    ~Bytes() { free(data); }
};
bool cancelled(const std::atomic<bool> *flag) { return flag && flag->load(); }
uint8_t clip(int32_t value) {
    return static_cast<uint8_t>(value < 0 ? 0 : value > 255 ? 255 : value);
}

// The HAL1 capture wrapper uses full-range RGB with these integer coefficients
// and top-left chroma sampling. Preserve that color conversion around the CPU
// engine; Filter_Process_YUV has a separate private conversion-thread ABI.
void toRgba(const QCamera3StockFilter::Image &image, uint8_t *rgba) {
    for (uint32_t row = 0; row < image.height; ++row) {
        const uint8_t *y = image.data + image.yOffset + size_t(row) * image.yStride;
        const uint8_t *uv = image.data + image.uvOffset + size_t(row / 2) * image.uvStride;
        for (uint32_t col = 0; col < image.width; ++col) {
            const int32_t v = int32_t(uv[col & ~1U]) - 128;
            const int32_t u = int32_t(uv[(col & ~1U) + 1]) - 128;
            uint8_t *out = rgba + (size_t(row) * image.width + col) * 4;
            out[0] = clip((int32_t(y[col]) * 128 + 176 * v + 64) >> 7);
            out[1] = clip((int32_t(y[col]) * 128 - 89 * v - 43 * u + 64) >> 7);
            out[2] = clip((int32_t(y[col]) * 128 + 222 * u + 64) >> 7);
            out[3] = 255;
        }
    }
}
void toNv21(const uint8_t *rgba, uint32_t width, uint32_t height, uint8_t *nv21) {
    uint8_t *uv = nv21 + size_t(width) * height;
    for (uint32_t row = 0; row < height; ++row) {
        for (uint32_t col = 0; col < width; ++col) {
            const uint8_t *p = rgba + (size_t(row) * width + col) * 4;
            const int32_t r = p[0], g = p[1], b = p[2];
            nv21[size_t(row) * width + col] = clip((77 * r + 150 * g + 29 * b + 128) >> 8);
            if (!(row & 1) && !(col & 1)) {
                uv[size_t(row / 2) * width + col] = clip((131 * r - 110 * g - 22 * b + 32896) >> 8);
                uv[size_t(row / 2) * width + col + 1] = clip((-44 * r - 87 * g + 131 * b + 32896) >> 8);
            }
        }
    }
}
int32_t apply(uint8_t *rgba, uint32_t width, uint32_t height, int32_t type,
        int32_t flipMode, const std::atomic<bool> *cancelFlag) {
    if (cancelled(cancelFlag)) return -ECANCELED;
    Process process = api().process;
    if (!process) return -ENOSYS;
    const int32_t stride = int32_t(width * 4);
    const int32_t rc = process(type, rgba, rgba, int32_t(width), int32_t(height),
            stride, stride, 1, flipMode);
    if (rc != 0) return -EIO;
    return cancelled(cancelFlag) ? -ECANCELED : 0;
}
bool validOptions(int32_t type, int32_t flipMode) {
    return QCamera3StockFilter::supportsType(type) && flipMode >= 0 && flipMode <= 2;
}
} // namespace

bool QCamera3StockFilter::isAvailable() { return api().process != nullptr; }

bool QCamera3StockFilter::supportsType(int32_t type) {
    // CPU worker switch branches 2..5 return without touching pixels.
    return type == 0 || type == 1 || (type >= 6 && type <= 20);
}

int32_t QCamera3StockFilter::processRgba(uint8_t *rgba, size_t length, uint32_t width,
        uint32_t height, uint32_t stride, int32_t type, int32_t flipMode,
        const std::atomic<bool> *cancelFlag) {
    if (!validOptions(type, flipMode)) return -ENOTSUP;
    if (!rgba || !width || !height || width > INT32_MAX / 4 || height > INT32_MAX ||
            stride < width * 4 || uint64_t(stride) * (height - 1) + width * 4 > length)
        return -EINVAL;
    const uint64_t bytes = uint64_t(width) * height * 4;
    if (bytes > SIZE_MAX) return -EOVERFLOW;
    Bytes scratch{size_t(bytes)};
    if (!scratch.data) return -ENOMEM;
    for (uint32_t row = 0; row < height; ++row)
        memcpy(scratch.data + size_t(row) * width * 4, rgba + size_t(row) * stride, width * 4);
    const int32_t rc = apply(scratch.data, width, height, type, flipMode, cancelFlag);
    if (rc) return rc;
    for (uint32_t row = 0; row < height; ++row)
        memcpy(rgba + size_t(row) * stride, scratch.data + size_t(row) * width * 4, width * 4);
    return 0;
}

int32_t QCamera3StockFilter::process(const Image &image, int32_t type, int32_t flipMode,
        const std::atomic<bool> *cancelFlag) {
    if (!validOptions(type, flipMode)) return -ENOTSUP;
    if (!QCamera3HdrAlgo::validImage(image) || image.width > INT32_MAX / 4) return -EINVAL;
    const uint64_t pixelCount = uint64_t(image.width) * image.height;
    if (pixelCount * 4 > SIZE_MAX) return -EOVERFLOW;
    if (cancelled(cancelFlag)) return -ECANCELED;
    Bytes rgba{size_t(pixelCount * 4)};
    Bytes nv21{size_t(pixelCount * 3 / 2)};
    if (!rgba.data || !nv21.data) return -ENOMEM;
    toRgba(image, rgba.data);
    const int32_t rc = apply(rgba.data, image.width, image.height, type, flipMode, cancelFlag);
    if (rc) return rc;
    toNv21(rgba.data, image.width, image.height, nv21.data);
    if (cancelled(cancelFlag)) return -ECANCELED;
    for (uint32_t row = 0; row < image.height; ++row)
        memcpy(image.data + image.yOffset + size_t(row) * image.yStride,
                nv21.data + size_t(row) * image.width, image.width);
    for (uint32_t row = 0; row < image.height / 2; ++row)
        memcpy(image.data + image.uvOffset + size_t(row) * image.uvStride,
                nv21.data + size_t(pixelCount) + size_t(row) * image.width, image.width);
    return 0;
}
} // namespace qcamera
