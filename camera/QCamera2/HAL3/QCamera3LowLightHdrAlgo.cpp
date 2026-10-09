/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3LowLightHdrAlgo.h"

#include <cmath>
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
// LLHDR uses pointers, unlike the ordinary HDR core's inline records.
struct ArcInput { int32_t count; ArcImage *images[8]; };
struct ArcParam {
    int32_t intensity, lightIntensity, saturation, sharpenIntensity, internalFd;
};
using ProgressCallback = int32_t (*)(int32_t, int32_t, void *);
#if UINTPTR_MAX == UINT32_MAX
static_assert(sizeof(ArcImage) == 44, "ARC_LLHDR ARM32 image ABI");
static_assert(sizeof(ArcInput) == 36, "ARC_LLHDR ARM32 input ABI");
#endif
static_assert(sizeof(ArcParam) == 20, "ARC_LLHDR parameter ABI");

pthread_mutex_t gLowLightHdrLock = PTHREAD_MUTEX_INITIALIZER;
struct Lock {
    Lock() { pthread_mutex_lock(&gLowLightHdrLock); }
    ~Lock() { pthread_mutex_unlock(&gLowLightHdrLock); }
};
struct Api {
    void *library;
    int32_t (*init)(void **);
    int32_t (*uninit)(void **);
    int32_t (*defaults)(ArcParam *, int32_t, int32_t);
    void (*setDrc)(void *, float);
    void (*setCallback)(void *, ProgressCallback, void *);
    int32_t (*process)(void *, ArcInput *, void *, ArcImage *, int32_t *,
            ArcParam *, int32_t, int32_t);
    Api() : library(dlopen("libarcsoft_low_light_hdr.so", RTLD_NOW | RTLD_LOCAL)),
            init(nullptr), uninit(nullptr), defaults(nullptr), setDrc(nullptr),
            setCallback(nullptr), process(nullptr) {
        if (!library) return;
        init = reinterpret_cast<decltype(init)>(dlsym(library, "ARC_LLHDR_Init"));
        uninit = reinterpret_cast<decltype(uninit)>(dlsym(library, "ARC_LLHDR_Uninit"));
        defaults = reinterpret_cast<decltype(defaults)>(dlsym(library, "ARC_LLHDR_GetDefaultParam"));
        setDrc = reinterpret_cast<decltype(setDrc)>(dlsym(library, "ARC_LLHDR_SetDRCGain"));
        setCallback = reinterpret_cast<decltype(setCallback)>(dlsym(library, "ARC_LLHDR_SetCallback"));
        process = reinterpret_cast<decltype(process)>(dlsym(library, "ARC_LLHDR_Process"));
    }
    ~Api() { if (library) dlclose(library); }
    bool valid() const {
        return init && uninit && defaults && setDrc && setCallback && process;
    }
};
struct Scratch {
    uint8_t *data[6] = {};
    ~Scratch() { for (unsigned i = 0; i < 6; ++i) free(data[i]); }
};
struct Cancellation { const std::atomic<bool> *flag; };
bool cancelled(const std::atomic<bool> *flag) { return flag && flag->load(); }
int32_t progress(int32_t, int32_t, void *user) {
    return cancelled(static_cast<Cancellation *>(user)->flag) ? 1 : 0;
}
void copyPlane(uint8_t *dst, uint32_t dstStride, const uint8_t *src,
        uint32_t srcStride, uint32_t width, uint32_t rows) {
    for (uint32_t row = 0; row < rows; ++row)
        memcpy(dst + size_t(row) * dstStride, src + size_t(row) * srcStride, width);
}
} // namespace

bool QCamera3LowLightHdrAlgo::isAvailable() {
    Lock lock;
    Api api;
    return api.valid();
}

int32_t QCamera3LowLightHdrAlgo::process(const Image images[5], const Image &dst,
        int32_t iso, float drcGain, int32_t cameraType,
        const std::atomic<bool> *cancelFlag) {
    if (!images || iso <= 0 || !std::isfinite(drcGain) || drcGain <= 0 ||
            (cameraType != 0 && cameraType != 1) || !QCamera3HdrAlgo::validImage(dst))
        return -EINVAL;
    for (unsigned i = 0; i < 5; ++i) {
        if (!QCamera3HdrAlgo::validImage(images[i]) || images[i].width != dst.width ||
                images[i].height != dst.height) return -EINVAL;
    }
    if (cancelled(cancelFlag)) return -ECANCELED;
    const uint32_t stride = (dst.width + 63) & ~63U;
    const uint64_t ySize = uint64_t(stride) * dst.height;
    const uint64_t size = ySize + uint64_t(stride) * (dst.height / 2);
    if (size > SIZE_MAX || size > UINT32_MAX) return -EOVERFLOW;
    Scratch scratch;
    ArcImage descriptors[6] = {};
    ArcInput input = {};
    input.count = 5;
    for (unsigned i = 0; i < 6; ++i) {
        scratch.data[i] = static_cast<uint8_t *>(calloc(1, size_t(size)));
        if (!scratch.data[i]) return -ENOMEM;
        ArcImage &image = descriptors[i];
        image.format = 0x802; // ASVL_PAF_NV21
        image.width = dst.width;
        image.height = dst.height;
        image.planes[0] = scratch.data[i];
        image.planes[1] = scratch.data[i] + size_t(ySize);
        image.pitches[0] = image.pitches[1] = stride;
        memset(image.planes[1], 128, size_t(size - ySize));
        if (i < 5) {
            input.images[i] = &image;
            copyPlane(image.planes[0], stride, images[i].data + images[i].yOffset,
                    images[i].yStride, dst.width, dst.height);
            copyPlane(image.planes[1], stride, images[i].data + images[i].uvOffset,
                    images[i].uvStride, dst.width, dst.height / 2);
        }
    }
    // Keep descriptors, scratch ownership, and callback token alive until
    // Uninit, even if the core reorders input.images or Init partially fails.
    Cancellation token{cancelFlag};
    Lock lock;
    Api api;
    if (!api.valid()) return -ENOSYS;
    if (cancelled(cancelFlag)) return -ECANCELED;
    void *handle = nullptr;
    int32_t rc = api.init(&handle);
    if (rc == 0 && !handle) rc = -EIO;
    ArcParam param = {};
    if (rc == 0 && cancelled(cancelFlag)) rc = -ECANCELED;
    if (rc == 0) rc = api.defaults(&param, iso, cameraType);
    if (rc == 0 && cancelled(cancelFlag)) rc = -ECANCELED;
    if (rc == 0) {
        param.intensity = 70;
        param.lightIntensity = 0;
        param.saturation = 50;
        param.sharpenIntensity = 40;
        api.setDrc(handle, drcGain);
        if (cancelled(cancelFlag)) rc = -ECANCELED;
        else api.setCallback(handle, progress, &token);
        if (cancelled(cancelFlag)) rc = -ECANCELED;
    }
    int32_t outputIndex = -1;
    if (rc == 0)
        rc = api.process(handle, &input, nullptr, &descriptors[5], &outputIndex,
                &param, iso, cameraType);
    if (handle) {
        const int32_t uninitRc = api.uninit(&handle);
        if (rc == 0) rc = uninitRc;
    }
    if (cancelled(cancelFlag)) return -ECANCELED;
    if (rc != 0) return rc;
    // A separate output makes outputIndex optional; stock leaves it at -1.
    const ArcImage &out = descriptors[5];
    if (out.format != 0x802 || out.width != int32_t(dst.width) ||
            out.height != int32_t(dst.height) ||
            out.planes[0] != scratch.data[5] ||
            out.planes[1] != scratch.data[5] + size_t(ySize) ||
            out.planes[2] || out.planes[3] ||
            out.pitches[0] != int32_t(stride) || out.pitches[1] != int32_t(stride) ||
            out.pitches[2] || out.pitches[3]) return -EPROTO;
    copyPlane(dst.data + dst.yOffset, dst.yStride, out.planes[0], stride,
            dst.width, dst.height);
    copyPlane(dst.data + dst.uvOffset, dst.uvStride, out.planes[1], stride,
            dst.width, dst.height / 2);
    return 0;
}
} // namespace qcamera
