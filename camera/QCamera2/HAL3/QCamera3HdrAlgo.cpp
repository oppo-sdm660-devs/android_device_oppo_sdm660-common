/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3HdrAlgo.h"

#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

namespace qcamera {
namespace {

// ARC_HDR inline three-image ABI (the hdr_denoise wrapper uses pointers).
struct ArcImage {
    uint32_t format;
    int32_t width, height;
    uint8_t *planes[4];
    int32_t pitches[4];
};
struct ArcInput { int32_t count; ArcImage images[3]; };
struct ArcParam { int32_t toneLength, brightness, saturation; };
#if UINTPTR_MAX == UINT32_MAX
static_assert(sizeof(ArcImage) == 44, "ARC_HDR ARM32 image ABI");
static_assert(sizeof(ArcInput) == 136, "ARC_HDR ARM32 input ABI");
#endif
static_assert(sizeof(ArcParam) == 12, "ARC_HDR parameter ABI");

// Init changes global camera flags; the lock covers the engine's whole lifetime.
pthread_mutex_t gHdrLock = PTHREAD_MUTEX_INITIALIZER;
struct Lock {
    Lock() { pthread_mutex_lock(&gHdrLock); }
    ~Lock() { pthread_mutex_unlock(&gHdrLock); }
};
struct Api {
    void *library;
    int32_t (*init)(void **, int32_t);
    int32_t (*defaults)(ArcParam *);
    int32_t (*preprocess)(void *, ArcInput *, void *, int32_t);
    int32_t (*process)(void *, ArcParam *, int32_t *);
    int32_t (*uninit)(void **);
    Api() : library(dlopen("libarcsoft_high_dynamic_range.so", RTLD_NOW | RTLD_LOCAL)),
            init(nullptr), defaults(nullptr), preprocess(nullptr),
            process(nullptr), uninit(nullptr) {
        if (!library) return;
        init = reinterpret_cast<decltype(init)>(dlsym(library, "ARC_HDR_Init"));
        defaults = reinterpret_cast<decltype(defaults)>(dlsym(library, "ARC_HDR_GetDefaultParam"));
        preprocess = reinterpret_cast<decltype(preprocess)>(dlsym(library, "ARC_HDR_PreProcess"));
        process = reinterpret_cast<decltype(process)>(dlsym(library, "ARC_HDR_Process"));
        uninit = reinterpret_cast<decltype(uninit)>(dlsym(library, "ARC_HDR_Uninit"));
    }
    ~Api() { if (library) dlclose(library); }
    bool valid() const { return init && defaults && preprocess && process && uninit; }
};
struct Scratch {
    uint8_t *data[3] = {nullptr, nullptr, nullptr};
    ~Scratch() { for (unsigned i = 0; i < 3; ++i) free(data[i]); }
};
bool cancelled(const std::atomic<bool> *flag) {
    return flag && flag->load();
}
bool planeValid(size_t offset, uint32_t stride, uint32_t scanline,
        uint32_t width, uint32_t rows, size_t length) {
    if (stride < width || scanline < rows || !rows) return false;
    const uint64_t end = uint64_t(offset) + uint64_t(stride) * (rows - 1) + width;
    return end >= offset && end <= length;
}
void copyPlane(uint8_t *dst, uint32_t dstStride, const uint8_t *src,
        uint32_t srcStride, uint32_t width, uint32_t rows) {
    for (uint32_t row = 0; row < rows; ++row)
        memcpy(dst + size_t(row) * dstStride, src + size_t(row) * srcStride, width);
}
} // namespace

bool QCamera3HdrAlgo::isAvailable() {
    Lock lock;
    Api api;
    return api.valid();
}

bool QCamera3HdrAlgo::validImage(const Image &im) {
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

int32_t QCamera3HdrAlgo::process(const Image images[3], const Image &dst,
        bool frontCamera, const std::atomic<bool> *cancelFlag) {
    if (!images || !validImage(dst)) return -EINVAL;
    for (unsigned i = 0; i < 3; ++i) {
        if (!validImage(images[i]) || images[i].width != dst.width ||
                images[i].height != dst.height) return -EINVAL;
    }
    if (cancelled(cancelFlag)) return -ECANCELED;
    const uint32_t stride = (dst.width + 63) & ~63U;
    const uint64_t ySize = uint64_t(stride) * dst.height;
    const uint64_t size = ySize + uint64_t(stride) * (dst.height / 2);
    if (size > SIZE_MAX || size > UINT32_MAX) return -EOVERFLOW;
    Scratch scratch;
    ArcInput input = {};
    input.count = 3;
    for (unsigned i = 0; i < 3; ++i) {
        scratch.data[i] = static_cast<uint8_t *>(calloc(1, size_t(size)));
        if (!scratch.data[i]) return -ENOMEM;
        ArcImage &out = input.images[i];
        out.format = 0x802; // ASVL_PAF_NV21
        out.width = dst.width;
        out.height = dst.height;
        out.planes[0] = scratch.data[i];
        out.planes[1] = scratch.data[i] + size_t(ySize);
        out.pitches[0] = out.pitches[1] = stride;
        memset(out.planes[1], 128, size_t(size - ySize));
        copyPlane(out.planes[0], stride, images[i].data + images[i].yOffset,
                images[i].yStride, dst.width, dst.height);
        copyPlane(out.planes[1], stride, images[i].data + images[i].uvOffset,
                images[i].uvStride, dst.width, dst.height / 2);
    }
    Lock lock;
    Api api;
    if (!api.valid()) return -ENOSYS;
    if (cancelled(cancelFlag)) return -ECANCELED;
    void *handle = nullptr;
    int32_t rc = api.init(&handle, frontCamera ? 0x100 : 0);
    ArcParam param = {};
    if (rc == 0 && !handle) rc = -EIO;
    if (rc == 0) rc = api.defaults(&param);
    for (int32_t i = 0; i < 3 && rc == 0; ++i) {
        if (cancelled(cancelFlag)) rc = -ECANCELED;
        else rc = api.preprocess(handle, &input, nullptr, i);
    }
    int32_t outputIndex = -1;
    if (rc == 0) {
        if (cancelled(cancelFlag)) rc = -ECANCELED;
        else rc = api.process(handle, &param, &outputIndex);
    }
    // A failed Init can still allocate a handle; keep inputs alive until Uninit.
    if (handle) {
        const int32_t uninitRc = api.uninit(&handle);
        if (rc == 0) rc = uninitRc;
    }
    if (rc != 0) return rc;
    if (cancelled(cancelFlag)) return -ECANCELED;
    if (outputIndex < 0 || outputIndex >= 3) return -ERANGE;
    const ArcImage &out = input.images[outputIndex];
    if (out.format != 0x802 || out.width != int32_t(dst.width) ||
            out.height != int32_t(dst.height) ||
            out.planes[0] != scratch.data[outputIndex] ||
            out.planes[1] != scratch.data[outputIndex] + size_t(ySize) ||
            out.pitches[0] != int32_t(stride) || out.pitches[1] != int32_t(stride))
        return -EPROTO;
    copyPlane(dst.data + dst.yOffset, dst.yStride, out.planes[0], stride,
            dst.width, dst.height);
    copyPlane(dst.data + dst.uvOffset, dst.uvStride, out.planes[1], stride,
            dst.width, dst.height / 2);
    return 0;
}
} // namespace qcamera
