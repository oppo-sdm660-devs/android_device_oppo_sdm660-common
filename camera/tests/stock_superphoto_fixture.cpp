/* SPDX-License-Identifier: BSD-3-Clause */
// ABI/call-sequence fixture. It does not implement VDSuperPhoto reconstruction.
#include "stock_superphoto_fixture.h"
#include <atomic>
#include <errno.h>
#include <stddef.h>
#include <string.h>

struct RenderParameters {
    int32_t nr, filter, sharp, detail, iso;
    float gain;
    int32_t reserved[4];
};
static_assert(sizeof(RenderParameters) == 40, "stock render ABI");
namespace {
SuperPhotoFixture stats;
struct Engine { uint8_t *y = nullptr, *uv = nullptr; int32_t base = -1; } engine;
}
extern "C" SuperPhotoFixture *superphoto_fixture() { return &stats; }
extern "C" void superphoto_reset() { stats = {}; engine = {}; }

void superZoomConfigPara(char *, float zoom, bool disableCrop, bool front,
        int32_t iso, void *parameters, int32_t) {
    stats.tuningZoom = zoom;
    stats.tuningDisableCrop = disableCrop;
    stats.tuningFront = front;
    stats.tuningIso = iso;
    static_cast<RenderParameters *>(parameters)->nr = 11;
}

extern "C" int32_t VDInitializeSuperPhotoWithCores(int32_t count, int32_t bits,
        int32_t width, int32_t height, int32_t left, int32_t top, float zoom,
        int32_t outWidth, int32_t outHeight, int32_t a, int32_t b,
        int32_t limitWidth, int32_t limitHeight, int32_t cores,
        int32_t *counter, void **handle) {
    ++stats.initCalls;
    stats.count = count; stats.width = width; stats.height = height;
    stats.outWidth = outWidth; stats.outHeight = outHeight; stats.initZoom = zoom;
    *counter = 0;
    *handle = &engine;
    if (bits != 8 || left || top || a != -1 || b != -1 || limitWidth != outWidth ||
            limitHeight != outHeight || cores != 8) return -EINVAL;
    return stats.failAt == 1 ? -EIO : 0;
}
extern "C" int32_t VDSuperPhoto_AddFramePlanar(uint8_t *y, uint8_t *uv, int32_t flag, void *) {
    if (stats.failAt == 2) return -EIO;
    const int32_t slot = stats.addedFrames++;
    if (slot >= 5 || flag) return -EINVAL;
    stats.inputs[slot] = reinterpret_cast<uintptr_t>(y);
    stats.firstLuma[slot] = y[0]; stats.firstV[slot] = uv[0]; stats.firstU[slot] = uv[1];
    // Some stock stages use input as scratch; caller images must still be immutable.
    memset(y, 0xDD, size_t(stats.width) * stats.height);
    return 0;
}
extern "C" int32_t VDSuperPhoto_AddOutputBufferPlanes(uint8_t *y, uint8_t *uv, void *) {
    ++stats.outputCalls;
    engine.y = y; engine.uv = uv;
    return stats.failAt == 3 ? -EIO : 0;
}
extern "C" int32_t VDSuperPhoto_Process(RenderParameters parameters, void *) {
    ++stats.processCalls;
    if (parameters.nr != 11 || parameters.iso != stats.tuningIso ||
            stats.addedFrames != stats.count) return -EINVAL;
    memset(engine.y, 0xAB, size_t(stats.outWidth) * stats.outHeight);
    memset(engine.uv, 0xCD, size_t(stats.outWidth) * stats.outHeight / 2);
    if (stats.cancelAfterProcess)
        reinterpret_cast<std::atomic<bool> *>(stats.cancelAfterProcess)->store(true);
    return stats.failAt == 4 ? -EIO : 0;
}
extern "C" int32_t VDReleaseSuperPhoto(void **handle) {
    ++stats.releaseCalls;
    *handle = nullptr;
    return stats.failAt == 5 ? -EIO : 0;
}
#ifndef SUPERPHOTO_OMIT_OUTPUT_QUERY
extern "C" int32_t VDSuperPhoto_GetOuputWidth(void *) {
    return stats.outWidth + (stats.failAt == 6 ? 2 : 0);
}
extern "C" int32_t VDSuperPhoto_GetOuputHeight(void *) { return stats.outHeight; }
#endif
#ifndef SUPERPHOTO_OMIT_BASE_FRAME
extern "C" int32_t VDSuperPhoto_SetBaseFrameIndex(int32_t base, void *) {
    engine.base = base;
    stats.baseIndex = base;
    return 0;
}
extern "C" int32_t VDSuperPhoto_GetBaseFrameIndex(void *) {
    return engine.base + (stats.failAt == 7 ? 1 : 0);
}
#endif
