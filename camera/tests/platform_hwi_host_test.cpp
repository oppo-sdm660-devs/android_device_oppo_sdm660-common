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
#include <stdlib.h>
#include <unistd.h>

extern "C" void host_fs_set_exists(const char *path, bool exists);
extern "C" void host_fs_clear_all(void);
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
    CHECK(maskForStillMode(5) == (kAlgoFaceBeauty | kAlgoFaceInfo));
    CHECK(maskForStillMode(7) == kAlgoBokeh);
    CHECK(maskForStillMode(9) == kAlgoVdSces);
    CHECK(maskForStillMode(10) == (kAlgoArcFaceBeauty | kAlgoFaceInfo));
    CHECK(maskForStillMode(0) == 0);
    CHECK(maskForStillMode(6) == 0);
    CHECK(maskForStillMode(11) == 0);
    // SCES(9)/front beauty(10)/rear beauty(5) are wired; SUPER(4) is accepted
    // only as a property-gated single-frame probe.
    CHECK(kSupportedPlatformMask == (kAlgoVdSces | kAlgoArcFaceBeauty |
            kAlgoFaceBeauty | kAlgoFaceInfo | kAlgoSuperPhoto));
    CHECK(isPlatformStillMode(5));
    CHECK(isPlatformStillMode(9));
    CHECK(isPlatformStillMode(10));
    CHECK(!isPlatformStillMode(1));
    CHECK(!isPlatformStillMode(2));
    CHECK(!isPlatformStillMode(3));
    CHECK(isPlatformStillMode(4));
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

    // P3 multi-image frame ctx: 0x40-byte slots, +0x38 primary byte, +0x280 count.
    uint8_t pixels1[100 * 50 * 3 / 2] = {};
    uint8_t pixels2[100 * 50 * 3 / 2] = {};
    Frame multi = frame;
    multi.data = pixels1;
    multi.length = sizeof(pixels1);
    multi.imageCount = 2;
    multi.images[0] = Image{pixels1, sizeof(pixels1), 100, 50, 100, 50};
    multi.images[1] = Image{pixels2, sizeof(pixels2), 100, 50, 100, 50};
    CHECK(buildFrameCtx(ctx, sizeof(ctx), multi));
    const size_t slot1 = FrameCtxOffsets::kImageSlotStride;
    CHECK(readU32(ctx, FrameCtxOffsets::kImageCount) == 2);
    CHECK(readPtr(ctx, FrameCtxOffsets::kImagePtr) == pixels1);
    CHECK(readPtr(ctx, slot1 + FrameCtxOffsets::kImagePtr) == pixels2);
    CHECK(readU32(ctx, slot1 + FrameCtxOffsets::kWidth) == 100);
    CHECK(readU32(ctx, slot1 + FrameCtxOffsets::kHeight) == 50);
    CHECK(readU32(ctx, slot1 + FrameCtxOffsets::kStride) == 100);
    CHECK(readU32(ctx, slot1 + FrameCtxOffsets::kImageFormat) == kFormatYuv420Nv21);
    CHECK(readU8(ctx, slot1 + FrameCtxOffsets::kImageValid) == 1);
    // Invalid or over-limit multi-frame input fails closed.
    multi.images[1].data = nullptr;
    CHECK(!buildFrameCtx(ctx, sizeof(ctx), multi));
    multi.imageCount = kMaxFrameImages + 1;
    CHECK(!buildFrameCtx(ctx, sizeof(ctx), multi));

    // Face block: inferred left/top/width/height arrays + count (descriptor 0).
    FaceInfo faces;
    faces.sourceWidth = 4000;
    faces.sourceHeight = 3000;
    faces.count = 2;
    faces.faces[0] = {1000, 750, 1000, 750};
    faces.faces[1] = {2000, 1500, 2000, 1500};
    CHECK(buildFaceInfo(param, sizeof(param), faces, 100, 50, false));
    const size_t faceBlock = ParamOffsets::kFaceInfoBase + ParamOffsets::kFaceBlockOffset;
    const size_t faceCount = ParamOffsets::kFaceInfoBase + ParamOffsets::kFaceCountOffset;
    CHECK(readU32(param, faceCount) == 2);
    CHECK(readU32(param, faceBlock + 0 * ParamOffsets::kFaceArrayStride) == 25);
    CHECK(readU32(param, faceBlock + 1 * ParamOffsets::kFaceArrayStride) == 12);
    CHECK(readU32(param, faceBlock + 2 * ParamOffsets::kFaceArrayStride) == 25);
    CHECK(readU32(param, faceBlock + 3 * ParamOffsets::kFaceArrayStride) == 12);
    CHECK(readU32(param, faceBlock + 4 * ParamOffsets::kFaceArrayStride) == 0);
    CHECK(readU32(param, faceBlock + 0 * ParamOffsets::kFaceArrayStride +
            sizeof(uint32_t)) == 50);
    CHECK(readU32(param, faceBlock + 2 * ParamOffsets::kFaceArrayStride +
            sizeof(uint32_t)) == 50);
    // Front camera mirror on X.
    CHECK(buildFaceInfo(param, sizeof(param), faces, 100, 50, true));
    CHECK(readU32(param, faceBlock + 0 * ParamOffsets::kFaceArrayStride) == 50);
    CHECK(readU32(param, faceBlock + 0 * ParamOffsets::kFaceArrayStride +
            sizeof(uint32_t)) == 0);
    // Invalid faces are skipped, count clamped to 10.
    FaceInfo many;
    many.sourceWidth = 4000;
    many.sourceHeight = 3000;
    many.count = 12;
    for (int i = 0; i < kMaxFaceInfo; ++i) many.faces[i] = {1000, 750, 1000, 750};
    CHECK(buildFaceInfo(param, sizeof(param), many, 100, 50, false));
    CHECK(readU32(param, faceCount) == 10);
    // count=0 clears the block and keeps unknown arrays zero.
    FaceInfo none;
    none.sourceWidth = 4000;
    none.sourceHeight = 3000;
    CHECK(buildFaceInfo(param, sizeof(param), none, 100, 50, false));
    CHECK(readU32(param, faceCount) == 0);

    // Beauty params (RE task-74): model path/strength/isFront/optional switches.
    Frame beautyFrame = frame;
    beautyFrame.beauty.enabled = true;
    beautyFrame.beauty.modelPath = "/tmp/does-not-matter.model";
    beautyFrame.beauty.strength = 60;
    beautyFrame.beauty.param24 = 3;
    beautyFrame.beauty.threshold1 = 0;
    beautyFrame.beauty.threshold2 = 0;
    beautyFrame.beauty.mode = 1;
    beautyFrame.beauty.enable = 1;
    beautyFrame.beauty.fdSelect = 2;
    beautyFrame.beauty.faceInfoMode = 4;
    beautyFrame.scesParam = 7;
    CHECK(buildParam(param, sizeof(param), beautyFrame));
    CHECK(strcmp(reinterpret_cast<const char *>(param) + ParamOffsets::kBeautyModelPath,
            "/tmp/does-not-matter.model") == 0);
    CHECK(readU32(param, ParamOffsets::kBeautyStrength) == 60);
    CHECK(readU32(param, ParamOffsets::kBeautyParam24) == 3);
    CHECK(readU8(param, ParamOffsets::kBeautyMode) == 1);
    CHECK(readU8(param, ParamOffsets::kBeautyEnable) == 1);
    CHECK(readU32(param, ParamOffsets::kFaceFdSelect) == 2);
    CHECK(readU32(param, ParamOffsets::kFaceInfoMode) == 4);
    CHECK(readU32(param, ParamOffsets::kScesParam) == 7);
    CHECK(param[ParamOffsets::kIsFront] == 1);
    // Strength is clamped to 0..100.
    beautyFrame.beauty.strength = 250;
    CHECK(buildParam(param, sizeof(param), beautyFrame));
    CHECK(readU32(param, ParamOffsets::kBeautyStrength) == 100);
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

    // 3. task-75 A/B: beauty params are pending (task-74) -> fail closed, no call.
    QCamera3OppoPlatformHwi::get().onOpenCamera(1, true);
    CHECK(!QCamera3OppoPlatformHwi::get().beautyParamsReady(10));
    CHECK(!QCamera3OppoPlatformHwi::get().beautyParamsReady(5));
    const int processBeforeBeauty = stats->processCount;
    CHECK(QCamera3OppoPlatformHwi::get().processStillFrame(10, frame) == -EOPNOTSUPP);
    CHECK(QCamera3OppoPlatformHwi::get().lastStatus() == -EOPNOTSUPP);
    CHECK(QCamera3OppoPlatformHwi::get().processStillFrame(5, frame) == -EOPNOTSUPP);
    CHECK(stats->processCount == processBeforeBeauty);
    CHECK(maskForStillMode(10) == (kAlgoArcFaceBeauty | kAlgoFaceInfo));
    CHECK(maskForStillMode(5) == (kAlgoFaceBeauty | kAlgoFaceInfo));

    // 3b. The shared face-block builder is still verified via SCENE(9), which
    // does not depend on beauty params or on consuming faces.
    QCamera3OppoPlatformFrame beauty = frame;
    beauty.faceInfo.count = 1;
    beauty.faceInfo.sourceWidth = 4000;
    beauty.faceInfo.sourceHeight = 3000;
    beauty.faceInfo.faces[0] = {1000, 750, 1000, 750};
    CHECK(QCamera3OppoPlatformHwi::get().processStillFrame(9, beauty) == 0);
    CHECK((stats->processMask & ~kInProcessBackend) == kAlgoVdSces);
    CHECK(stats->processParam[ParamOffsets::kIsFront] == 1);
    const size_t faceBlock = ParamOffsets::kFaceInfoBase + ParamOffsets::kFaceBlockOffset;
    const size_t faceCount = ParamOffsets::kFaceInfoBase + ParamOffsets::kFaceCountOffset;
    CHECK(readU32(stats->processParam, faceCount) == 1);
    // Front camera (still active): width 100, left 100-(25+25)=50.
    CHECK(readU32(stats->processParam, faceBlock) == 50);
    CHECK(readU32(stats->processParam, faceBlock + ParamOffsets::kFaceArrayStride) == 12);
    CHECK(readU32(stats->processParam,
            faceBlock + 2 * ParamOffsets::kFaceArrayStride) == 25);
    CHECK(readU32(stats->processParam,
            faceBlock + 3 * ParamOffsets::kFaceArrayStride) == 12);

    // 3c. Beauty: only runs when the model path exists (stat/property).
    char rearModel[] = "/tmp/oppo_beauty_rear_XXXXXX";
    char frontModel[] = "/tmp/oppo_beauty_front_XXXXXX";
    int rearFd = mkstemp(rearModel);
    int frontFd = mkstemp(frontModel);
    CHECK(rearFd >= 0 && frontFd >= 0);
    if (rearFd >= 0) close(rearFd);
    if (frontFd >= 0) close(frontFd);
    host_prop_set("persist.vendor.camera.oppo.platform_hwi.beauty_model", rearModel);
    host_prop_set("persist.vendor.camera.oppo.platform_hwi.beauty_model_front", frontModel);
    QCamera3OppoPlatformHwi::get().onOpenCamera(1, true);
    CHECK(QCamera3OppoPlatformHwi::get().beautyParamsReady(10));
    QCamera3OppoPlatformFrame front = frame;
    front.beautyStrength = 70;
    CHECK(QCamera3OppoPlatformHwi::get().processStillFrame(10, front) == 0);
    CHECK((stats->processMask & ~kInProcessBackend) ==
            (kAlgoArcFaceBeauty | kAlgoFaceInfo));
    CHECK(stats->processParam[ParamOffsets::kIsFront] == 1);
    CHECK(strcmp(reinterpret_cast<const char *>(stats->processParam) +
            ParamOffsets::kBeautyModelPath, frontModel) == 0);
    CHECK(readU32(stats->processParam, ParamOffsets::kBeautyStrength) == 70);
    QCamera3OppoPlatformHwi::get().onOpenCamera(0, false);
    CHECK(QCamera3OppoPlatformHwi::get().beautyParamsReady(5));
    QCamera3OppoPlatformFrame rear = frame;
    rear.beautyStrength = 40;
    CHECK(QCamera3OppoPlatformHwi::get().processStillFrame(5, rear) == 0);
    CHECK((stats->processMask & ~kInProcessBackend) ==
            (kAlgoFaceBeauty | kAlgoFaceInfo));
    CHECK(stats->processParam[ParamOffsets::kIsFront] == 0);
    CHECK(strcmp(reinterpret_cast<const char *>(stats->processParam) +
            ParamOffsets::kBeautyModelPath, rearModel) == 0);
    CHECK(readU32(stats->processParam, ParamOffsets::kBeautyStrength) == 40);
    // Without an existing path/property the mode fails closed again.
    host_prop_clear_all();
    QCamera3OppoPlatformHwi::get().onOpenCamera(0, false);
    CHECK(!QCamera3OppoPlatformHwi::get().beautyParamsReady(5));
    const int processAfterBeauty = stats->processCount;
    CHECK(QCamera3OppoPlatformHwi::get().processStillFrame(5, rear) == -EOPNOTSUPP);
    CHECK(stats->processCount == processAfterBeauty);
    unlink(rearModel);
    unlink(frontModel);
    // Restore the run environment cleared above (host_prop_clear_all also unsets path).
    host_prop_set("persist.vendor.camera.oppo.platform_hwi", "1");
    host_prop_set("persist.vendor.camera.oppo.platform_hwi.run", "1");
    host_prop_set("persist.vendor.camera.oppo.platform_hwi.path", argv[1]);

    // 3d. SCES strength -> param+0x10bc (mode 9 only; clamp; property A/B).
    QCamera3OppoPlatformFrame scene = frame;
    scene.sceneStrength = 42;
    CHECK(QCamera3OppoPlatformHwi::get().processStillFrame(9, scene) == 0);
    CHECK(readU32(stats->processParam, ParamOffsets::kScesParam) == 42);
    scene.sceneStrength = 250;
    CHECK(QCamera3OppoPlatformHwi::get().processStillFrame(9, scene) == 0);
    CHECK(readU32(stats->processParam, ParamOffsets::kScesParam) == 100);
    scene.sceneStrength = 0;
    CHECK(QCamera3OppoPlatformHwi::get().processStillFrame(9, scene) == 0);
    CHECK(readU32(stats->processParam, ParamOffsets::kScesParam) == 0);
    host_prop_set("persist.vendor.camera.oppo.platform_hwi.sces_param", "7");
    QCamera3OppoPlatformHwi::get().onOpenCamera(0, false);
    scene.sceneStrength = 42;
    CHECK(QCamera3OppoPlatformHwi::get().processStillFrame(9, scene) == 0);
    CHECK(readU32(stats->processParam, ParamOffsets::kScesParam) == 7);
    host_prop_set("persist.vendor.camera.oppo.platform_hwi.sces_param", "");
    QCamera3OppoPlatformHwi::get().onOpenCamera(0, false);
    CHECK(QCamera3OppoPlatformHwi::get().processStillFrame(9, scene) == 0);
    CHECK(readU32(stats->processParam, ParamOffsets::kScesParam) == 42);
    // 3e. Model path candidates: /etc file > /system file > /system dir > none.
    host_prop_set("persist.vendor.camera.oppo.platform_hwi.beauty_model", "");
    host_prop_set("persist.vendor.camera.oppo.platform_hwi.beauty_model_front", "");
    host_fs_clear_all();
    host_fs_set_exists("/system/etc/camera/gasryuv.model", true);
    QCamera3OppoPlatformHwi::get().onOpenCamera(0, false);
    CHECK(QCamera3OppoPlatformHwi::get().beautyParamsReady(5));
    QCamera3OppoPlatformFrame modelFrame = frame;
    modelFrame.beautyStrength = 10;
    CHECK(QCamera3OppoPlatformHwi::get().processStillFrame(5, modelFrame) == 0);
    CHECK(strcmp(reinterpret_cast<const char *>(stats->processParam) +
            ParamOffsets::kBeautyModelPath, "/system/etc/camera/gasryuv.model") == 0);
    host_fs_set_exists("/etc/camera/gasryuv.model", true);
    QCamera3OppoPlatformHwi::get().onOpenCamera(0, false);
    CHECK(QCamera3OppoPlatformHwi::get().processStillFrame(5, modelFrame) == 0);
    CHECK(strcmp(reinterpret_cast<const char *>(stats->processParam) +
            ParamOffsets::kBeautyModelPath, "/etc/camera/gasryuv.model") == 0);
    host_fs_clear_all();
    host_fs_set_exists("/system/etc/camera", true);
    QCamera3OppoPlatformHwi::get().onOpenCamera(0, false);
    CHECK(QCamera3OppoPlatformHwi::get().processStillFrame(5, modelFrame) == 0);
    CHECK(strcmp(reinterpret_cast<const char *>(stats->processParam) +
            ParamOffsets::kBeautyModelPath, "/system/etc/camera") == 0);
    host_fs_clear_all();
    QCamera3OppoPlatformHwi::get().onOpenCamera(0, false);
    CHECK(!QCamera3OppoPlatformHwi::get().beautyParamsReady(5));
    // Property override still wins when both it and a candidate exist.
    host_fs_set_exists("/etc/camera/gasryuv.model", true);
    char propModel[] = "/tmp/oppo_beauty_prop_XXXXXX";
    int propFd = mkstemp(propModel);
    CHECK(propFd >= 0);
    if (propFd >= 0) close(propFd);
    host_prop_set("persist.vendor.camera.oppo.platform_hwi.beauty_model", propModel);
    QCamera3OppoPlatformHwi::get().onOpenCamera(0, false);
    CHECK(QCamera3OppoPlatformHwi::get().beautyParamsReady(5));
    CHECK(QCamera3OppoPlatformHwi::get().processStillFrame(5, modelFrame) == 0);
    CHECK(strcmp(reinterpret_cast<const char *>(stats->processParam) +
            ParamOffsets::kBeautyModelPath, propModel) == 0);
    unlink(propModel);
    host_prop_set("persist.vendor.camera.oppo.platform_hwi.beauty_model", "");
    host_fs_clear_all();

    // 3f. SUPER(4) single-frame probe: property-gated, needs .update, registers
    // the bit only for a new session (init mask decides what the core executes).
    host_prop_set("persist.vendor.camera.oppo.platform_hwi.super", "1");
    host_prop_set("persist.vendor.camera.oppo.platform_hwi.update", "1");
    QCamera3OppoPlatformHwi::shutdownOnClose();
    const int initBeforeSuper = stats->initCount;
    QCamera3OppoPlatformHwi::get().onOpenCamera(0, false);
    CHECK(QCamera3OppoPlatformHwi::get().processStillFrame(4, frame) == 0);
    CHECK(stats->initCount == initBeforeSuper + 1);
    CHECK((stats->initMask & ~kInProcessBackend) & kAlgoSuperPhoto);
    CHECK((stats->processMask & ~kInProcessBackend) == kAlgoSuperPhoto);
    CHECK(stats->updateCount == 1);
    // Turning the property off fails closed before the library.
    host_prop_set("persist.vendor.camera.oppo.platform_hwi.super", "0");
    const int processBeforeSuperOff = stats->processCount;
    CHECK(QCamera3OppoPlatformHwi::get().processStillFrame(4, frame) == -EOPNOTSUPP);
    CHECK(stats->processCount == processBeforeSuperOff);

    // 4. HDR/LLHDR/MFNR/bokeh are not wired: fail closed before the library.
    const int processBeforeUnwired = stats->processCount;
    const int updateBeforeUnwired = stats->updateCount;
    host_prop_set("persist.vendor.camera.oppo.platform_hwi.update", "1");
    for (int mode : {1, 2, 3, 4, 7}) {
        CHECK(QCamera3OppoPlatformHwi::get().processStillFrame(mode, frame) == -EOPNOTSUPP);
    }
    CHECK(stats->processCount == processBeforeUnwired);
    CHECK(stats->updateCount == updateBeforeUnwired);

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

    // 8. Face result cache: exact frame, bounded recent frame, never stale.
    QCamera3OppoPlatformFaceInfo cached;
    cached.count = 1;
    cached.sourceWidth = 4000;
    cached.sourceHeight = 3000;
    cached.faces[0] = {1000, 750, 1000, 750};
    QCamera3OppoPlatformHwi::setFaceInfo(100, cached);
    QCamera3OppoPlatformFaceInfo out;
    CHECK(QCamera3OppoPlatformHwi::getFaceInfo(100, out) && out.count == 1);
    CHECK(QCamera3OppoPlatformHwi::getFaceInfo(110, out) && out.count == 1);
    CHECK(!QCamera3OppoPlatformHwi::getFaceInfo(100 + 31, out) && out.count == 0);
    CHECK(!QCamera3OppoPlatformHwi::getFaceInfo(99, out));

    // 9. Close uninitializes the platform block exactly once for this session.
    const int uninitBeforeClose = stats->uninitCount;
    QCamera3OppoPlatformHwi::shutdownOnClose();
    CHECK(stats->uninitCount == uninitBeforeClose + 1);

    if (g_failures) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("oppo platform host tests: all checks passed\n");
    return 0;
}
