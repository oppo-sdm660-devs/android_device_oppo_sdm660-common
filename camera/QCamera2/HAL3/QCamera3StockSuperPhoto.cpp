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
    int32_t (*outputWidth)(void *) = nullptr;
    int32_t (*outputHeight)(void *) = nullptr;
    int32_t (*setBaseFrame)(int32_t, void *) = nullptr;
    int32_t (*baseFrame)(void *) = nullptr;
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
        outputWidth = reinterpret_cast<decltype(outputWidth)>(dlsym(core, "VDSuperPhoto_GetOuputWidth"));
        outputHeight = reinterpret_cast<decltype(outputHeight)>(dlsym(core, "VDSuperPhoto_GetOuputHeight"));
        setBaseFrame = reinterpret_cast<decltype(setBaseFrame)>(dlsym(core, "VDSuperPhoto_SetBaseFrameIndex"));
        baseFrame = reinterpret_cast<decltype(baseFrame)>(dlsym(core, "VDSuperPhoto_GetBaseFrameIndex"));
        configure = reinterpret_cast<decltype(configure)>(dlsym(tuning,
                "_Z19superZoomConfigParaPcfbbiPvi"));
    }
    ~Api() {
        if (tuning) dlclose(tuning);
        if (core) dlclose(core);
    }
    bool valid() const { return initialize && addFrame && addOutput && process && release && configure; }
    bool zoomValid() const { return valid() && outputWidth && outputHeight && setBaseFrame && baseFrame; }
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
using Image = QCamera3StockSuperPhoto::Image;
using Source = QCamera3StockSuperPhoto::Source;
bool validSources(const Source *sources, uint32_t count) {
    if (!sources) return false;
    const Source &reference = sources[0];
    for (uint32_t i = 0; i < count; ++i) {
        const Source &source = sources[i];
        if (source.timestampNs <= 0 || source.exposureTimeNs <= 0 || source.sensitivity <= 0)
            return false;
        for (uint32_t j = 0; j < i; ++j)
            if (source.frameNumber == sources[j].frameNumber ||
                    source.timestampNs == sources[j].timestampNs) return false;
        if (double(source.exposureTimeNs) < double(reference.exposureTimeNs) * .85 ||
                double(source.exposureTimeNs) > double(reference.exposureTimeNs) * 1.15 ||
                source.sensitivity != reference.sensitivity) return false;
    }
    return true;
}
int32_t processCore(const Image *images, uint32_t count, const Image &output,
        const char *sensorName, int32_t iso, bool frontCamera, float zoom,
        const std::atomic<bool> *cancelled, bool highResolution = false) {
    if (!images || (highResolution ? count != QCamera3StockSuperPhoto::HIGH_RESOLUTION_FRAME_COUNT :
            count < 2 || count > QCamera3StockSuperPhoto::ZOOM_FRAME_COUNT) ||
            !QCamera3HdrAlgo::validImage(output) ||
            !sensorName || sensorName[0] == '\0' || strnlen(sensorName, 32) == 32 || iso <= 0)
        return -EINVAL;
    const uint32_t inputWidth = images[0].width, inputHeight = images[0].height;
    for (uint32_t i = 0; i < count; ++i)
        if (!QCamera3HdrAlgo::validImage(images[i]) || images[i].width != inputWidth ||
                images[i].height != inputHeight) return -EINVAL;
    const uint32_t scale = highResolution ? 2 : 1;
    if (uint64_t(inputWidth) * scale != output.width ||
            uint64_t(inputHeight) * scale != output.height) return -EINVAL;
    if (stopped(cancelled)) return -ECANCELED;
    // Stock packs NV21 tightly; keep caller inputs immutable and commit atomically.
    const uint64_t inputYBytes = uint64_t(inputWidth) * inputHeight;
    const uint64_t inputFrameBytes = inputYBytes + inputYBytes / 2;
    const uint64_t outputYBytes = uint64_t(output.width) * output.height;
    const uint64_t outputFrameBytes = outputYBytes + outputYBytes / 2;
    // Five 4608x3456 inputs + one 9216x6912 output need 205 MiB. The engine's
    // own allocations and upstream buffers remain outside this scratch budget.
    const uint64_t scratchBudget = (highResolution ? 256U : 128U) * 1024U * 1024U;
    if (outputFrameBytes > scratchBudget ||
            inputFrameBytes > (scratchBudget - outputFrameBytes) / count) return -EOVERFLOW;
    const uint64_t workingBytes = inputFrameBytes * count + outputFrameBytes;
    if (workingBytes > SIZE_MAX ||
            output.width > INT_MAX || output.height > INT_MAX ||
            inputWidth > INT_MAX || inputHeight > INT_MAX) return -EOVERFLOW;
    Scratch scratch;
    scratch.planes = static_cast<uint8_t *>(calloc(1, size_t(workingBytes)));
    if (!scratch.planes) return -ENOMEM;
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t *image = scratch.planes + size_t(inputFrameBytes) * i;
        copyPlane(image, inputWidth, images[i].data + images[i].yOffset,
                images[i].yStride, inputWidth, inputHeight);
        copyPlane(image + size_t(inputYBytes), inputWidth, images[i].data + images[i].uvOffset,
                images[i].uvStride, inputWidth, inputHeight / 2);
    }
    uint8_t *destination = scratch.planes + size_t(inputFrameBytes) * count;
    Lock lock;
    Api api;
    const bool superZoom = zoom > 1.0f;
    const bool verifyOutput = superZoom || highResolution;
    if (verifyOutput ? !api.zoomValid() : !api.valid()) return -ENOSYS;
    if (stopped(cancelled)) return -ECANCELED;
    char sensor[32] = {};
    memcpy(sensor, sensorName, strlen(sensorName));
    // Defaults at the stock caller's 0xff60, then the exact stock tuning call.
    RenderParameters parameters = {0, 0, 0, 0, iso, -1.0f, {0, 0, 0, 60}};
    api.configure(sensor, zoom, false, frontCamera, iso, &parameters, 0);
    if (stopped(cancelled)) return -ECANCELED;
    void *engine = nullptr;
    int32_t frameCounter = 0;
    int32_t rc = api.initialize(count, 8, inputWidth, inputHeight, 0, 0,
            zoom, output.width, output.height, -1, -1, output.width,
            output.height, 8, &frameCounter, &engine);
    if (rc == 0 && !engine) rc = -EIO;
    if (rc == 0 && verifyOutput &&
            (api.outputWidth(engine) != int32_t(output.width) ||
             api.outputHeight(engine) != int32_t(output.height))) rc = -EPROTO;
    if (rc == 0 && verifyOutput) rc = api.setBaseFrame(0, engine);
    for (uint32_t i = 0; i < count && rc == 0; ++i) {
        uint8_t *image = scratch.planes + size_t(inputFrameBytes) * i;
        rc = stopped(cancelled) ? -ECANCELED :
                api.addFrame(image, image + size_t(inputYBytes), 0, engine);
    }
    if (rc == 0) rc = stopped(cancelled) ? -ECANCELED :
            api.addOutput(destination, destination + size_t(outputYBytes), engine);
    if (rc == 0) rc = stopped(cancelled) ? -ECANCELED : api.process(parameters, engine);
    if (rc == 0 && verifyOutput && api.baseFrame(engine) != 0) rc = -EPROTO;
    if (engine) {
        const int32_t cleanup = api.release(&engine);
        if (rc == 0) rc = cleanup;
    }
    if (rc != 0) return rc;
    if (stopped(cancelled)) return -ECANCELED;
    copyPlane(output.data + output.yOffset, output.yStride, destination, output.width,
            output.width, output.height);
    copyPlane(output.data + output.uvOffset, output.uvStride, destination + size_t(outputYBytes),
            output.width, output.width, output.height / 2);
    return 0;
}
}

bool QCamera3StockSuperPhoto::isAvailable() {
    Lock lock;
    Api api;
    return api.valid();
}

bool QCamera3StockSuperPhoto::is2xAvailable() {
    Lock lock;
    Api api;
    return api.zoomValid();
}

bool QCamera3StockSuperPhoto::isHighResolutionAvailable() {
    Lock lock;
    Api api;
    return api.zoomValid();
}

int32_t QCamera3StockSuperPhoto::process(const Image *images, uint32_t count,
        const Image &output, const char *sensorName, int32_t iso, bool frontCamera,
        const std::atomic<bool> *cancelled) {
    return processCore(images, count, output, sensorName, iso, frontCamera, 1.0f, cancelled);
}

int32_t QCamera3StockSuperPhoto::process2x(const Image *images, const Source *sources,
        uint32_t count, const Image &output, const char *sensorName,
        const ZoomOptions &options, ZoomResult *result, const std::atomic<bool> *cancelled) {
    if (!options.fullFieldOfView || count != ZOOM_FRAME_COUNT ||
            !validSources(sources, count)) return -EINVAL;
    const Source &reference = sources[0];
    const int32_t rc = processCore(images, count, output, sensorName,
            reference.sensitivity, false, 2.0f, cancelled);
    if (rc != 0) return rc;
    if (result) {
        ZoomResult report;
        report.algorithmApplied = true;
        report.zoomRatio = 2.0f;
        report.submittedFrames = count;
        memcpy(report.sources, sources, sizeof(report.sources));
        *result = report;
    }
    return 0;
}

int32_t QCamera3StockSuperPhoto::processHighResolution(const Image *images,
        const Source *sources, uint32_t count, const Image &output, const char *sensorName,
        const HighResolutionOptions &options, HighResolutionResult *result,
        const std::atomic<bool> *cancelled) {
    if (!options.fullFieldOfView || count != HIGH_RESOLUTION_FRAME_COUNT ||
            !validSources(sources, count)) return -EINVAL;
    const int32_t rc = processCore(images, count, output, sensorName,
            sources[0].sensitivity, false, 1.0f, cancelled, true);
    if (rc != 0) return rc;
    if (result) {
        HighResolutionResult report;
        report.algorithmApplied = true;
        report.submittedFrames = count;
        memcpy(report.sources, sources, sizeof(report.sources));
        *result = report;
    }
    return 0;
}
}
