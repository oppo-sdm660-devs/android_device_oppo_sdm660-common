/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Host behavior test for the reversed OPPO platform wiring:
 *  - QCamera3OppoPlatformParams mask/offset builders
 *  - QCamera3OppoPlatformHwi gate -> init -> updateparameters -> process order
 *  - per-stillMode algorithm mask, param/frame-ctx layout and failure reporting
 * The fake liboppo_platform_hwi.so (argv[1]) records every call.
 */
#include "QCamera3OppoPlatformHwi.h"
#include "QCamera3OppoPlatformParams.h"
#include "fake_oppo_platform_hwi.h"
#include "cutils/properties.h"

#include <dlfcn.h>
#include <errno.h>
#include <initializer_list>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

using namespace qcamera::oppo_platform;

namespace {

int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

struct Harness {
    FakeOppoPlatformStats *(*stats)() = nullptr;
    void (*reset)() = nullptr;
    void (*setProcessRc)(int) = nullptr;
    void (*setInitRc)(int) = nullptr;
    void *handle = nullptr;

    bool load(const char *path) {
        handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (handle == nullptr) {
            fprintf(stderr, "dlopen(%s): %s\n", path, dlerror());
            return false;
        }
        stats = reinterpret_cast<FakeOppoPlatformStats *(*)()>(
                dlsym(handle, "fake_oppo_platform_stats"));
        reset = reinterpret_cast<void (*)()>(
                dlsym(handle, "fake_oppo_platform_reset"));
        setProcessRc = reinterpret_cast<void (*)(int)>(
                dlsym(handle, "fake_oppo_platform_set_process_rc"));
        setInitRc = reinterpret_cast<void (*)(int)>(
                dlsym(handle, "fake_oppo_platform_set_init_rc"));
        return stats && reset && setProcessRc && setInitRc;
    }
};

void testParamsUnit() {
    CHECK(maskForStillMode(1) == kAlgoHdr);
    CHECK(maskForStillMode(2) == kAlgoLlhdr);
    CHECK(maskForStillMode(3) == kAlgoPicBest);
    CHECK(maskForStillMode(4) == kAlgoSuperPhoto);
    CHECK(maskForStillMode(5) == kAlgoFaceBeauty);
    CHECK(maskForStillMode(7) == kAlgoBokeh);
    CHECK(maskForStillMode(9) == kAlgoVdSces);
    CHECK(maskForStillMode(10) == kAlgoArcFaceBeauty);
    CHECK(maskForStillMode(0) == 0);
    CHECK(maskForStillMode(6) == 0);
    CHECK(maskForStillMode(11) == 0);
    // Only SCES(9)/front beauty(10)/rear beauty(5) are wired to platform.
    CHECK(kSupportedPlatformMask == (kAlgoVdSces | kAlgoArcFaceBeauty | kAlgoFaceBeauty));
    CHECK(isPlatformStillMode(5));
    CHECK(isPlatformStillMode(9));
    CHECK(isPlatformStillMode(10));
    CHECK(!isPlatformStillMode(1));
    CHECK(!isPlatformStillMode(2));
    CHECK(!isPlatformStillMode(3));
    CHECK(!isPlatformStillMode(4));
    CHECK(!isPlatformStillMode(7));
    CHECK(!isPlatformStillMode(0));
    CHECK(supportedMask(kAlgoVdSces));
    CHECK(supportedMask(kAlgoHdr | kAlgoLlhdr));
    CHECK(!supportedMask(0));
    CHECK(!supportedMask(1ULL << 30));
    CHECK(!supportedMask(kInProcessBackend));
    CHECK(strcmp(algorithmName(kAlgoVdSces), "VDSces") == 0);
    CHECK(strcmp(algorithmName(kAlgoArcFaceBeauty), "ArcFaceBeauty") == 0);
    CHECK(strcmp(algorithmName(1ULL << 30), "Unknown") == 0);

    uint8_t param[qcamera::oppo_platform::kParamSize] = {};
    uint8_t ctx[qcamera::oppo_platform::kFrameCtxSize] = {};
    uint8_t pixels[100 * 50 * 3 / 2] = {};
    uint8_t scratch[qcamera::oppo_platform::kScratchBytes] = {};
    Frame frame;
    frame.data = pixels;
    frame.length = sizeof(pixels);
    frame.width = 100;
    frame.height = 50;
    frame.stride = 128;
    frame.slice = 64;
    frame.isFront = 1;
    frame.jpegRotation = 270;
    CHECK(buildParam(param, sizeof(param), frame));
    CHECK(readU32(param, ParamOffsets::kWidth) == 100);
    CHECK(readU32(param, ParamOffsets::kHeight) == 50);
    CHECK(readU32(param, ParamOffsets::kStride) == 128);
    CHECK(readU32(param, ParamOffsets::kSlice) == 64);
    CHECK(param[ParamOffsets::kIsFront] == 1);
    CHECK(readU32(param, ParamOffsets::kJpegRotation) == 270);
    CHECK(param[ParamOffsets::kFaceInfoBase] == 0);
    CHECK(buildFrameCtx(ctx, sizeof(ctx), frame));
    CHECK(readU32(ctx, FrameCtxOffsets::kWidth) == 100);
    CHECK(readU32(ctx, FrameCtxOffsets::kHeight) == 50);
    CHECK(readU32(ctx, FrameCtxOffsets::kStride) == 128);
    CHECK(readU32(ctx, FrameCtxOffsets::kSlice) == 64);
    CHECK(readPtr(ctx, FrameCtxOffsets::kImagePtr) == pixels);
    CHECK(readU32(ctx, FrameCtxOffsets::kImageFormat) == kFormatYuv420Nv21);
    CHECK(readU8(ctx, FrameCtxOffsets::kImageValid) == 1);
    CHECK(readU32(ctx, FrameCtxOffsets::kImageCount) == 1);
    // No scratch -> the three descriptor fields stay zero.
    CHECK(readU32(ctx, FrameCtxOffsets::kScratchFd) == 0);
    CHECK(readPtr(ctx, FrameCtxOffsets::kScratchPtr) == nullptr);
    CHECK(readU32(ctx, FrameCtxOffsets::kScratchSize) == 0);
    frame.scratchFd = 7;
    frame.scratchPtr = scratch;
    frame.scratchSize = sizeof(scratch);
    CHECK(buildFrameCtx(ctx, sizeof(ctx), frame));
    CHECK(readU32(ctx, FrameCtxOffsets::kScratchFd) == 7);
    CHECK(readPtr(ctx, FrameCtxOffsets::kScratchPtr) == scratch);
    CHECK(readU32(ctx, FrameCtxOffsets::kScratchSize) == sizeof(scratch));
    // Fail closed on bad geometry or a too-small block.
    Frame bad = frame;
    bad.width = 0;
    CHECK(!buildParam(param, sizeof(param), bad));
    CHECK(!buildFrameCtx(ctx, 0x10, frame));
}

QCamera3OppoPlatformFrame makeFrame(uint8_t *data) {
    QCamera3OppoPlatformFrame frame = {};
    frame.data = data;
    frame.length = 100 * 50 * 3 / 2;
    frame.width = 100;
    frame.height = 50;
    frame.stride = 128;
    frame.slice = 64;
    return frame;
}

}  // namespace

extern "C" void host_stub_log(const char *level, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "[%s] ", level);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <fake liboppo_platform_hwi.so>\n", argv[0]);
        return 2;
    }
    testParamsUnit();

    Harness harness;
    if (!harness.load(argv[1])) return 2;
    FakeOppoPlatformStats *stats = harness.stats();
    uint8_t data[100 * 50 * 3 / 2] = {};
    const QCamera3OppoPlatformFrame frame = makeFrame(data);

    // 1. Default gates: no library load, no call, clear fallback status.
    host_prop_clear_all();
    CHECK(!QCamera3OppoPlatformHwi::configured());
    CHECK(!QCamera3OppoPlatformHwi::runEnabled());
    CHECK(QCamera3OppoPlatformHwi::get().processStillFrame(9, frame) == -EOPNOTSUPP);
    CHECK(stats->initCount == 0);
    CHECK(stats->processCount == 0);

    // 2. Enable the run path; init happens on the first real frame.
    host_prop_set("persist.vendor.camera.oppo.platform_hwi", "1");
    host_prop_set("persist.vendor.camera.oppo.platform_hwi.run", "1");
    host_prop_set("persist.vendor.camera.oppo.platform_hwi.path", argv[1]);
    CHECK(QCamera3OppoPlatformHwi::configured());
    CHECK(QCamera3OppoPlatformHwi::runEnabled());
    QCamera3OppoPlatformHwi::get().onOpenCamera(0, false);
    CHECK(stats->initCount == 0);
    harness.reset();
    CHECK(QCamera3OppoPlatformHwi::get().processStillFrame(9, frame) == 0);
    CHECK(stats->initCount == 1);
    CHECK(stats->processCount == 1);
    CHECK((stats->initMask & ~kInProcessBackend) == QCamera3OppoPlatformHwi::sessionAlgorithmMask());
    CHECK((stats->initMask & kInProcessBackend) != 0);
    CHECK((stats->processMask & ~kInProcessBackend) == kAlgoVdSces);
    CHECK((stats->processMask & kInProcessBackend) != 0);
    CHECK(readU32(stats->processParam, ParamOffsets::kWidth) == 100);
    CHECK(readU32(stats->processParam, ParamOffsets::kHeight) == 50);
    CHECK(readU32(stats->processParam, ParamOffsets::kStride) == 128);
    CHECK(readU32(stats->processParam, ParamOffsets::kSlice) == 64);
    CHECK(stats->processParam[ParamOffsets::kIsFront] == 0);
    CHECK(readU32(stats->frameCtx, FrameCtxOffsets::kWidth) == 100);
    CHECK(readU32(stats->frameCtx, FrameCtxOffsets::kHeight) == 50);
    CHECK(readU32(stats->frameCtx, FrameCtxOffsets::kStride) == 128);
    CHECK(readU32(stats->frameCtx, FrameCtxOffsets::kSlice) == 64);
    CHECK(readPtr(stats->frameCtx, FrameCtxOffsets::kImagePtr) == frame.data);
    CHECK(readU32(stats->frameCtx, FrameCtxOffsets::kImageFormat) == kFormatYuv420Nv21);
    CHECK(readU8(stats->frameCtx, FrameCtxOffsets::kImageValid) == 1);
    CHECK(readU32(stats->frameCtx, FrameCtxOffsets::kImageCount) == 1);
    // Scratch is allocated by the HWI (session-level) and never left NULL.
    CHECK(readPtr(stats->frameCtx, FrameCtxOffsets::kScratchPtr) != nullptr);
    CHECK(readU32(stats->frameCtx, FrameCtxOffsets::kScratchSize) ==
            qcamera::oppo_platform::kScratchBytes);
    CHECK(readU32(stats->frameCtx, FrameCtxOffsets::kScratchFd) == 0xFFFFFFFFu);

    // 3. Front beauty uses the ARC face-beauty bit and the front flag.
    QCamera3OppoPlatformHwi::get().onOpenCamera(1, true);
    CHECK(QCamera3OppoPlatformHwi::get().processStillFrame(10, frame) == 0);
    CHECK((stats->processMask & ~kInProcessBackend) == kAlgoArcFaceBeauty);
    CHECK(stats->processParam[ParamOffsets::kIsFront] == 1);

    // 3b. Rear beauty uses the face-beauty bit; front flag cleared again.
    CHECK(QCamera3OppoPlatformHwi::get().processStillFrame(5, frame) == 0);
    CHECK((stats->processMask & ~kInProcessBackend) == kAlgoFaceBeauty);
    CHECK(stats->processParam[ParamOffsets::kIsFront] == 1);

    // 4. HDR/LLHDR/MFNR/SUPER/bokeh are not wired: fail closed before the library.
    const int processBeforeUnwired = stats->processCount;
    CHECK(stats->updateCount == 0);
    host_prop_set("persist.vendor.camera.oppo.platform_hwi.update", "1");
    for (int mode : {1, 2, 3, 4, 7}) {
        CHECK(QCamera3OppoPlatformHwi::get().processStillFrame(mode, frame) == -EOPNOTSUPP);
    }
    CHECK(stats->processCount == processBeforeUnwired);
    CHECK(stats->updateCount == 0);

    // 5. Algorithm failure is reported, not hidden as success.
    harness.setProcessRc(-EIO);
    CHECK(QCamera3OppoPlatformHwi::get().processStillFrame(9, frame) == -EIO);
    CHECK(QCamera3OppoPlatformHwi::get().lastStatus() == -EIO);
    harness.setProcessRc(0);

    // 6. Modes outside the wired set fail closed (fallback path).
    const int processBefore = stats->processCount;
    CHECK(QCamera3OppoPlatformHwi::get().processStillFrame(0, frame) == -EOPNOTSUPP);
    CHECK(stats->processCount == processBefore);
    CHECK(QCamera3OppoPlatformHwi::get().processStillFrame(6, frame) == -EOPNOTSUPP);
    CHECK(stats->processCount == processBefore);

    // 7. Invalid frame geometry fails before touching the library.
    QCamera3OppoPlatformFrame invalid = frame;
    invalid.width = 0;
    CHECK(QCamera3OppoPlatformHwi::get().processStillFrame(9, invalid) == -EINVAL);

    // 8. Close uninitializes the platform block exactly once.
    QCamera3OppoPlatformHwi::shutdownOnClose();
    CHECK(stats->uninitCount == 1);

    if (g_failures) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("oppo platform host tests: all checks passed\n");
    return 0;
}
