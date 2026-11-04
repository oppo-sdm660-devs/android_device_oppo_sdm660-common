/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3FrontBeauty.h"

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

namespace qcamera {
namespace {

// ARM32 ASVLOFFSCREEN as built by the stock caller: format, width, height,
// four plane pointers (4 bytes each on this ABI), four pitches.
struct ArcImage {
    uint32_t format;
    int32_t width, height;
    uint8_t *planes[4];
    int32_t pitches[4];
};
#if UINTPTR_MAX == UINT32_MAX
static_assert(sizeof(ArcImage) == 44, "ARC_BS_IMAGE ARM32 descriptor");
#endif

// The exported ARC_BS_IMAGE_Process only accepts u32PixelArrayFormat 0x605 and
// converts it to its internal planar 0x601 descriptor by swapping plane1/plane2;
// 0x605 is therefore the Y/V/U planar layout (YV12) for this core. 0x802 (NV21)
// belongs to the other ArcSoft API group and is not accepted here.
const uint32_t kYv12Format = 0x605;

// Feature ids resolved with ARC_BS_IMAGE_SetFeatureLevel by the stock caller
// (0xc9/0xec/0xca/0xcb/0xcd/0xcc/0xf0). Which beauty feature each id drives is
// not established, so the requested strength is applied to all of them.
const int32_t kFeatureIds[] = {0xc9, 0xec, 0xca, 0xcb, 0xcd, 0xcc, 0xf0};

pthread_mutex_t gFrontBeautyLock = PTHREAD_MUTEX_INITIALIZER;
struct Lock {
    Lock() { pthread_mutex_lock(&gFrontBeautyLock); }
    ~Lock() { pthread_mutex_unlock(&gFrontBeautyLock); }
};

struct Api {
    void *library = nullptr;
    // ARC_BS_IMAGE_Init takes no argument and returns the handle directly.
    void *(*init)() = nullptr;
    int32_t (*uninit)(void *) = nullptr;
    // No defined return value; the stock caller ignores it.
    void *(*setFeature)(void *, int32_t, int32_t) = nullptr;
    // (handle, source image, destination image, face hint, reserved).
    int32_t (*process)(void *, ArcImage *, ArcImage *, void *, void *) = nullptr;
    Api() : library(dlopen("libarcsoft_beautyshot.so", RTLD_NOW | RTLD_LOCAL)) {
        if (!library) return;
#define LOAD(member, symbol) member = reinterpret_cast<decltype(member)>(dlsym(library, symbol))
        LOAD(init, "ARC_BS_IMAGE_Init");
        LOAD(uninit, "ARC_BS_IMAGE_UnInit");
        LOAD(setFeature, "ARC_BS_IMAGE_SetFeatureLevel");
        LOAD(process, "ARC_BS_IMAGE_Process");
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
    for (uint32_t row = 0; row < rows; ++row)
        memcpy(dst + size_t(row) * dstStride, src + size_t(row) * srcStride, width);
}

bool sameImage(const ArcImage &actual, const ArcImage &expected) {
    if (actual.format != expected.format || actual.width != expected.width ||
            actual.height != expected.height) return false;
    for (unsigned i = 0; i < 4; ++i)
        if (actual.planes[i] != expected.planes[i] || actual.pitches[i] != expected.pitches[i])
            return false;
    return true;
}

// NV21 UV row (V U V U ...) -> separate V and U planes.
void splitChroma(const uint8_t *src, uint32_t srcStride, uint8_t *v, uint8_t *u,
        uint32_t chromaStride, uint32_t chromaWidth, uint32_t rows) {
    for (uint32_t row = 0; row < rows; ++row) {
        const uint8_t *in = src + size_t(row) * srcStride;
        uint8_t *vRow = v + size_t(row) * chromaStride;
        uint8_t *uRow = u + size_t(row) * chromaStride;
        for (uint32_t col = 0; col < chromaWidth; ++col) {
            vRow[col] = in[2 * col];
            uRow[col] = in[2 * col + 1];
        }
    }
}

void joinChroma(uint8_t *dst, uint32_t dstStride, const uint8_t *v, const uint8_t *u,
        uint32_t chromaStride, uint32_t chromaWidth, uint32_t rows) {
    for (uint32_t row = 0; row < rows; ++row) {
        uint8_t *out = dst + size_t(row) * dstStride;
        const uint8_t *vRow = v + size_t(row) * chromaStride;
        const uint8_t *uRow = u + size_t(row) * chromaStride;
        for (uint32_t col = 0; col < chromaWidth; ++col) {
            out[2 * col] = vRow[col];
            out[2 * col + 1] = uRow[col];
        }
    }
}

} // namespace

bool QCamera3FrontBeauty::isAvailable() {
    Lock lock;
    Api api;
    return api.valid();
}

int32_t QCamera3FrontBeauty::process(const Image &image, int32_t strength,
        const std::atomic<bool> *cancelFlag) {
    if (!QCamera3HdrAlgo::validImage(image) || strength < 0 || strength > 100)
        return -EINVAL;
    // A planar YUV core needs an even luma stride and a chroma pitch of yStride/2.
    if (image.yStride & 1U) return -EINVAL;
    const uint32_t chromaStride = image.yStride / 2;
    const uint32_t chromaWidth = image.width / 2;
    const uint32_t chromaRows = image.height / 2;
    const uint64_t ySize = uint64_t(image.yStride) * image.height;
    const uint64_t chromaSize = uint64_t(chromaStride) * chromaRows;
    const uint64_t length = ySize + 2 * chromaSize;
    // Bound the transient YV12 image in this 32-bit camera service.
    if (length > 64U * 1024U * 1024U || length > SIZE_MAX) return -E2BIG;
    if (stopped(cancelFlag)) return -ECANCELED;

    Buffer memory{size_t(length)};
    if (!memory.data) return -ENOMEM;
    uint8_t *yPlane = memory.data;
    uint8_t *vPlane = memory.data + size_t(ySize);
    uint8_t *uPlane = vPlane + size_t(chromaSize);
    copyRows(yPlane, image.yStride, image.data + image.yOffset, image.yStride,
            image.width, image.height);
    splitChroma(image.data + image.uvOffset, image.uvStride, vPlane, uPlane,
            chromaStride, chromaWidth, chromaRows);

    ArcImage frame = {};
    frame.format = kYv12Format;
    frame.width = static_cast<int32_t>(image.width);
    frame.height = static_cast<int32_t>(image.height);
    frame.planes[0] = yPlane;
    frame.planes[1] = vPlane;
    frame.planes[2] = uPlane;
    frame.pitches[0] = static_cast<int32_t>(image.yStride);
    frame.pitches[1] = static_cast<int32_t>(chromaStride);
    frame.pitches[2] = static_cast<int32_t>(chromaStride);
    const ArcImage expected = frame;

    Lock lock;
    Api api;
    if (!api.valid()) return -ENOSYS;
    if (stopped(cancelFlag)) return -ECANCELED;
    void *handle = api.init();
    if (!handle) return -EIO;
    int32_t rc = 0;
    for (size_t i = 0; i < sizeof(kFeatureIds) / sizeof(kFeatureIds[0]); ++i) {
        if (stopped(cancelFlag)) { rc = -ECANCELED; break; }
        api.setFeature(handle, kFeatureIds[i], strength);
    }
    // No face hint is passed: the 0x284-byte face record the stock caller builds
    // is not decoded, and passing a fabricated record would be a layout guess.
    if (!rc) rc = stopped(cancelFlag) ? -ECANCELED : api.process(handle, &frame, &frame,
            nullptr, nullptr);
    const int32_t cleanup = api.uninit(handle);
    if (!rc) rc = cleanup;
    if (rc) return rc;
    if (stopped(cancelFlag)) return -ECANCELED;
    // In-place processing must keep the caller-provided descriptor.
    if (!sameImage(frame, expected)) return -EPROTO;

    copyRows(image.data + image.yOffset, image.yStride, yPlane, image.yStride,
            image.width, image.height);
    joinChroma(image.data + image.uvOffset, image.uvStride, vPlane, uPlane,
            chromaStride, chromaWidth, chromaRows);
    return 0;
}

} // namespace qcamera
