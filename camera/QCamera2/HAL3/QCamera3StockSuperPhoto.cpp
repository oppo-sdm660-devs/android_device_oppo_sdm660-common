/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3StockSuperPhoto.h"
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

namespace qcamera {
namespace {
// R11/R11s superphoto_Process takes 40 bytes at sp+0x90: first four words in
// r0-r3, the rest on the stack; superZoomConfigPara writes the ten words.
struct RenderParameters {
    int32_t noiseReduction, colorFilter, sharpen, detail;
    int32_t iso;
    float gain;
    int32_t reserved[4];
};
static_assert(sizeof(RenderParameters) == 40, "VDSuperPhoto render ABI");
pthread_mutex_t gSuperPhotoLock = PTHREAD_MUTEX_INITIALIZER;
struct Lock {
    Lock() { pthread_mutex_lock(&gSuperPhotoLock); }
    ~Lock() { pthread_mutex_unlock(&gSuperPhotoLock); }
};
struct Api {
    void *core = dlopen("libVDSuperPhotoAPI.so", RTLD_NOW | RTLD_LOCAL);
    void *tuning = nullptr;
    int32_t (*initialize)(int32_t, int32_t, int32_t, int32_t, int32_t, int32_t,
            float, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t,
            int32_t *, void **) = nullptr;
    int32_t (*addFrame)(uint8_t *, uint8_t *, int32_t, void *) = nullptr;
    int32_t (*addOutput)(uint8_t *, uint8_t *, void *) = nullptr;
    int32_t (*process)(RenderParameters, void *) = nullptr;
    int32_t (*release)(void **) = nullptr;
    void (*configure)(char *, float, bool, bool, int32_t, void *, int32_t) = nullptr;
    Api() {
        if (!core) return;
        tuning = dlopen("libcamera_core_hwi.so", RTLD_NOW | RTLD_LOCAL);
        if (!tuning) return;
        initialize = reinterpret_cast<decltype(initialize)>(dlsym(core, "VDInitializeSuperPhotoWithCores"));
        addFrame = reinterpret_cast<decltype(addFrame)>(dlsym(core, "VDSuperPhoto_AddFramePlanar"));
        addOutput = reinterpret_cast<decltype(addOutput)>(dlsym(core, "VDSuperPhoto_AddOutputBufferPlanes"));
        process = reinterpret_cast<decltype(process)>(dlsym(core, "VDSuperPhoto_Process"));
        release = reinterpret_cast<decltype(release)>(dlsym(core, "VDReleaseSuperPhoto"));
        configure = reinterpret_cast<decltype(configure)>(dlsym(tuning,
                "_Z19superZoomConfigParaPcfbbiPvi"));
    }
    ~Api() {
        if (tuning) dlclose(tuning);
        if (core) dlclose(core);
    }
    bool valid() const { return initialize && addFrame && addOutput && process && release && configure; }
};
struct Scratch {
    uint8_t *planes = nullptr;
    ~Scratch() { free(planes); }
};
bool stopped(const std::atomic<bool> *cancelled) { return cancelled && cancelled->load(); }
void copyPlane(uint8_t *dst, uint32_t dstStride, const uint8_t *src, uint32_t srcStride,
        uint32_t width, uint32_t rows) {
    for (uint32_t y = 0; y < rows; ++y)
        memcpy(dst + size_t(y) * dstStride, src + size_t(y) * srcStride, width);
}
}

bool QCamera3StockSuperPhoto::isAvailable() {
    Lock lock;
    Api api;
    return api.valid();
}

int32_t QCamera3StockSuperPhoto::process(const Image *images, uint32_t count,
        const Image &output, const char *sensorName, int32_t iso, bool frontCamera,
        const std::atomic<bool> *cancelled) {
    if (!images || count < 2 || count > 4 || !QCamera3HdrAlgo::validImage(output) ||
            !sensorName || sensorName[0] == '\0' || strnlen(sensorName, 32) == 32 || iso <= 0)
        return -EINVAL;
    for (uint32_t i = 0; i < count; ++i)
        if (!QCamera3HdrAlgo::validImage(images[i]) || images[i].width != output.width ||
                images[i].height != output.height) return -EINVAL;
    if (stopped(cancelled)) return -ECANCELED;
    // Stock packs NV21 tightly; keep caller inputs immutable and commit atomically.
    const uint64_t yBytes = uint64_t(output.width) * output.height;
    const uint64_t frameBytes = yBytes + yBytes / 2;
    const uint64_t scratchBudget = 128U * 1024U * 1024U;
    if (frameBytes > scratchBudget / (count + 1)) return -EOVERFLOW;
    const uint64_t workingBytes = frameBytes * (count + 1);
    if (workingBytes > SIZE_MAX ||
            output.width > INT_MAX || output.height > INT_MAX) return -EOVERFLOW;
    Scratch scratch;
    scratch.planes = static_cast<uint8_t *>(calloc(1, size_t(workingBytes)));
    if (!scratch.planes) return -ENOMEM;
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t *image = scratch.planes + size_t(frameBytes) * i;
        copyPlane(image, output.width, images[i].data + images[i].yOffset,
                images[i].yStride, output.width, output.height);
        copyPlane(image + size_t(yBytes), output.width, images[i].data + images[i].uvOffset,
                images[i].uvStride, output.width, output.height / 2);
    }
    uint8_t *destination = scratch.planes + size_t(frameBytes) * count;
    Lock lock;
    Api api;
    if (!api.valid()) return -ENOSYS;
    if (stopped(cancelled)) return -ECANCELED;
    char sensor[32] = {};
    memcpy(sensor, sensorName, strlen(sensorName));
    // Defaults at the stock caller's 0xff60, then the exact stock tuning call.
    RenderParameters parameters = {0, 0, 0, 0, iso, -1.0f, {0, 0, 0, 60}};
    api.configure(sensor, 1.0f, false, frontCamera, iso, &parameters, 0);
    if (stopped(cancelled)) return -ECANCELED;
    void *engine = nullptr;
    int32_t frameCounter = 0;
    int32_t rc = api.initialize(count, 8, output.width, output.height, 0, 0,
            1.0f, output.width, output.height, -1, -1, output.width,
            output.height, 8, &frameCounter, &engine);
    if (rc == 0 && !engine) rc = -EIO;
    for (uint32_t i = 0; i < count && rc == 0; ++i) {
        uint8_t *image = scratch.planes + size_t(frameBytes) * i;
        rc = stopped(cancelled) ? -ECANCELED : api.addFrame(image, image + size_t(yBytes), 0, engine);
    }
    if (rc == 0) rc = stopped(cancelled) ? -ECANCELED :
            api.addOutput(destination, destination + size_t(yBytes), engine);
    if (rc == 0) rc = stopped(cancelled) ? -ECANCELED : api.process(parameters, engine);
    if (engine) {
        const int32_t cleanup = api.release(&engine);
        if (rc == 0) rc = cleanup;
    }
    if (rc != 0) return rc;
    if (stopped(cancelled)) return -ECANCELED;
    copyPlane(output.data + output.yOffset, output.yStride, destination, output.width,
            output.width, output.height);
    copyPlane(output.data + output.uvOffset, output.uvStride, destination + size_t(yBytes),
            output.width, output.width, output.height / 2);
    return 0;
}
}
