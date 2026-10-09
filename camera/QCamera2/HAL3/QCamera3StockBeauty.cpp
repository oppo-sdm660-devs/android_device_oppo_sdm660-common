/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3StockBeauty.h"

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

namespace qcamera {
namespace {
struct ArcImage {
    uint32_t format;
    int32_t width, height;
    uint8_t *planes[4];
    int32_t pitches[4];
};
#if UINTPTR_MAX == UINT32_MAX
static_assert(sizeof(ArcImage) == 44, "ARCRLP ARM32 image ABI");
#endif

pthread_mutex_t gStockBeautyLock = PTHREAD_MUTEX_INITIALIZER;
struct Lock {
    Lock() { pthread_mutex_lock(&gStockBeautyLock); }
    ~Lock() { pthread_mutex_unlock(&gStockBeautyLock); }
};
struct Api {
    void *library = nullptr;
    void *(*init)(int32_t) = nullptr;
    int32_t (*uninit)(void *) = nullptr;
    void (*setFeature)(void *, int32_t, int32_t) = nullptr;
    int32_t (*process)(void *, ArcImage *, ArcImage *) = nullptr;
    Api() : library(dlopen("libarcsoft_relighting_pro_image.so", RTLD_NOW | RTLD_LOCAL)) {
        if (!library) return;
#define LOAD(member, symbol) member = reinterpret_cast<decltype(member)>(dlsym(library, symbol))
        LOAD(init, "ARCRLP_Init");
        LOAD(uninit, "ARCRLP_UnInit");
        LOAD(setFeature, "ARCRLP_SetFeatureLevel");
        LOAD(process, "ARCRLP_Process");
#undef LOAD
    }
    ~Api() { if (library) dlclose(library); }
    bool valid() const { return init && uninit && setFeature && process; }
};
struct Buffer {
    uint8_t *data;
    explicit Buffer(size_t length) : data(static_cast<uint8_t *>(calloc(1, length))) {}
    ~Buffer() { free(data); }
};
bool stopped(const std::atomic<bool> *flag) { return flag && flag->load(); }
void copyRows(uint8_t *dst, uint32_t dstStride, const uint8_t *src,
        uint32_t srcStride, uint32_t width, uint32_t rows) {
    for (uint32_t y = 0; y < rows; ++y)
        memcpy(dst + size_t(y) * dstStride, src + size_t(y) * srcStride, width);
}
bool sameImage(const ArcImage &actual, const ArcImage &expected) {
    if (actual.format != expected.format || actual.width != expected.width ||
            actual.height != expected.height) return false;
    for (unsigned i = 0; i < 4; ++i)
        if (actual.planes[i] != expected.planes[i] || actual.pitches[i] != expected.pitches[i])
            return false;
    return true;
}
} // namespace

bool QCamera3StockBeauty::isAvailable() {
    Lock lock;
    Api api;
    return api.valid();
}

int32_t QCamera3StockBeauty::process(const Image &image, const Options &options,
        const std::atomic<bool> *cancelFlag) {
    if (!QCamera3HdrAlgo::validImage(image) || options.soften > 100 ||
            options.softenSharp > 100 || options.skinBright > 100 || options.skinStyle > 100)
        return -EINVAL;
    if (stopped(cancelFlag)) return -ECANCELED;

    const uint32_t stride = (image.width + 63) & ~63U;
    const uint64_t ySize = uint64_t(stride) * image.height;
    const uint64_t length = ySize + ySize / 2;
    // Bound the writable image allocation in this 32-bit camera service.
    if (length > 64U * 1024U * 1024U || length > SIZE_MAX) return -E2BIG;
    Buffer memory{size_t(length)};
    if (!memory.data) return -ENOMEM;
    ArcImage frame = {};
    frame.format = 0x802;
    frame.width = image.width;
    frame.height = image.height;
    frame.planes[0] = memory.data;
    frame.planes[1] = memory.data + size_t(ySize);
    frame.pitches[0] = frame.pitches[1] = stride;
    memset(frame.planes[1], 128, size_t(ySize / 2));
    copyRows(frame.planes[0], stride, image.data + image.yOffset,
            image.yStride, image.width, image.height);
    copyRows(frame.planes[1], stride, image.data + image.uvOffset,
            image.uvStride, image.width, image.height / 2);
    const ArcImage expected = frame;

    Lock lock;
    Api api;
    if (!api.valid()) return -ENOSYS;
    if (stopped(cancelFlag)) return -ECANCELED;
    // BackFaceBeauty Init(0) returns the handle directly, unlike the HDR APIs.
    void *handle = api.init(0);
    if (!handle) return -EIO;
    int32_t rc = 0;
    const int32_t ids[] = {201, 231, 234, 229};
    const int32_t levels[] = {options.soften, options.skinBright,
            options.skinStyle, options.softenSharp};
    for (unsigned i = 0; i < 4; ++i) {
        if (stopped(cancelFlag)) { rc = -ECANCELED; break; }
        // SetFeatureLevel has no defined return value; stock ignores it.
        api.setFeature(handle, ids[i], levels[i]);
    }
    if (!rc) rc = stopped(cancelFlag) ? -ECANCELED : api.process(handle, &frame, &frame);
    const int32_t cleanup = api.uninit(handle);
    if (!rc) rc = cleanup;
    if (rc) return rc;
    if (stopped(cancelFlag)) return -ECANCELED;
    if (!sameImage(frame, expected)) return -EPROTO;

    copyRows(image.data + image.yOffset, image.yStride, memory.data,
            stride, image.width, image.height);
    copyRows(image.data + image.uvOffset, image.uvStride, memory.data + size_t(ySize),
            stride, image.width, image.height / 2);
    return 0;
}
} // namespace qcamera
