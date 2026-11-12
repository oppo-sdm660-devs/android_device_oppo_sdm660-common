/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3OppoPlatformHwi.h"
#include "QCamera3OppoPlatformParams.h"
#include "QCamera3FrontBeauty.h"
#include "QCamera3StockBeauty.h"

namespace oppo_platform = qcamera::oppo_platform;

/* mm_camera_dbg.h lacks its own extern "C" block; the symbol has C linkage. */
extern "C" {
#include "mm_camera_dbg.h"
}

#include <cutils/properties.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vector>

/* In-process wiring of the OPPO platform algorithm layer. The layer keeps one
 * .bss singleton per algorithm, so one process-wide mutex covers every call. */

namespace {

const char *const kOppoHwiProbeProp = "persist.vendor.camera.oppo.platform_hwi";
const char *const kOppoHwiProcessProp =
        "persist.vendor.camera.oppo.platform_hwi.process";
const char *const kOppoHwiMaskProp = "persist.vendor.camera.oppo.platform_hwi.mask";
const char *const kOppoHwiPathProp = "persist.vendor.camera.oppo.platform_hwi.path";
const char *const kOppoHwiDumpProp = "persist.vendor.camera.oppo.platform_hwi.dump";
const char *const kOppoHwiCallbackProp = "persist.vendor.camera.oppo.platform_hwi.callback";
const char *const kOppoHwiRunProp = "persist.vendor.camera.oppo.platform_hwi.run";
const char *const kOppoHwiUpdateProp = "persist.vendor.camera.oppo.platform_hwi.update";
/* SUPER(4) single-frame probe gate; requires .update=1 as well (RE task-98 P3). */
const char *const kOppoHwiSuperProp = "persist.vendor.camera.oppo.platform_hwi.super";
/* Beauty params (RE task-74). Model paths must exist; overrides are A/B.
 * Optional switches/thresholds are D-level and default to 0 (never invented). */
const char *const kOppoHwiBeautyModelProp =
        "persist.vendor.camera.oppo.platform_hwi.beauty_model";
const char *const kOppoHwiBeautyModelFrontProp =
        "persist.vendor.camera.oppo.platform_hwi.beauty_model_front";
const char *const kOppoHwiBeautyParam24Prop =
        "persist.vendor.camera.oppo.platform_hwi.beauty_param24";
const char *const kOppoHwiBeautyModeProp =
        "persist.vendor.camera.oppo.platform_hwi.beauty_mode";
const char *const kOppoHwiBeautyEnableProp =
        "persist.vendor.camera.oppo.platform_hwi.beauty_enable";
const char *const kOppoHwiFaceFdSelectProp =
        "persist.vendor.camera.oppo.platform_hwi.face_fd_select";
const char *const kOppoHwiFaceInfoModeProp =
        "persist.vendor.camera.oppo.platform_hwi.face_info_mode";
const char *const kOppoHwiScesParamProp =
        "persist.vendor.camera.oppo.platform_hwi.sces_param";
/* task-79: core hardcodes /etc/camera paths; /etc is system/etc on stock (HAL1
 * uses it directly). Prefix with the actual file first, then the /system path,
 * then diagnostic fallbacks. The source tag is logged for real-device A/B. */
struct OppoBeautyModelCandidate {
    const char *path;
    const char *source;
};
const OppoBeautyModelCandidate kOppoHwiBeautyModelCandidates[] = {
    {"/etc/camera/gasryuv.model", "etc-file"},
    {"/etc/camera/Charming.cng", "etc-file"},
    {"/etc/camera/skinSeg.model", "etc-file"},
    {"/etc/camera/faceTpl0.rgb32", "etc-file"},
    {"/system/etc/camera/gasryuv.model", "system-file"},
    {"/system/etc/camera", "system-dir"},
    {"/vendor/etc/camera", "vendor-dir"},
    {"/data/vendor/camera", "data-dir"},
};
const char *const kOppoHwiDumpDir = "/data/vendor/camera";
const char *const kOppoHwiDefaultPath = "/vendor/lib/liboppo_platform_hwi.so";
/* HDR = bit7, LLHDR = bit14; the runtime mask stays overridable. */
const uint64_t kOppoHwiDefaultMask = 0x4080ULL;

/* bit63 = 1 selects the in-process backend instead of the binder client. */
const uint64_t kOppoHwiInProcessBackend = 1ULL << 63;

const size_t kOppoHwiParamSize = 0x12EC;
const size_t kOppoHwiFrameCtxSize = 0x308;

/* param_platform_t offsets. */
const size_t kParamWidth = 0x000;
const size_t kParamHeight = 0x004;
const size_t kParamStride = 0x008;
const size_t kParamSlice = 0x00c;
const size_t kParamFlipMode = 0x054;
const size_t kParamVisiontechFilterType = 0x160;
const size_t kParamIsFront = 0x168;          /* u8 */
const size_t kParamOppoCapModeB = 0x169;     /* u8 */
const size_t kParamVendorCustomApi = 0x171;  /* u8 */
const size_t kParamPortraitLightMode = 0x10c0;
const size_t kParamJpegRotation = 0x10c4;
const size_t kParamOarNeedStop = 0x10e0;

/* Scratch descriptor required by core facebeauty/imageadjust/superphoto/blurless. */
const size_t kOppoHwiScratchSize = 0x1000;
const size_t kOppoHwiScratchAlign = 0x1000;
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

/* memfd so the algorithm can see the scratch through fd+ptr; -1 when the platform
 * (or host tests) cannot provide one. The core dereferences the pointer only. */
int oppoHwiCreateScratchFd(size_t size)
{
#if defined(__ANDROID__) && defined(__ANDROID_API__) && (__ANDROID_API__ >= 30)
    const int fd = memfd_create("oppo_hwi_scratch", MFD_CLOEXEC);
    if (fd < 0) return -1;
    if (ftruncate(fd, off_t(size)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
#else
    (void)size;
    return -1;
#endif
}

bool sOppoHwiCreated = false;

/* Face result cache shared with PostProc; lock-protected, keyed by internal frame. */
struct OppoFaceCacheEntry {
    uint32_t frame = 0;
    bool valid = false;
    QCamera3OppoPlatformFaceInfo info;
};
constexpr size_t kOppoFaceCacheSlots = 8;
constexpr uint32_t kOppoFaceMaxAgeFrames = 30;
OppoFaceCacheEntry sOppoFaceCache[kOppoFaceCacheSlots];
size_t sOppoFaceNext = 0;
pthread_mutex_t sOppoFaceLock = PTHREAD_MUTEX_INITIALIZER;

/* notify/data channels of the platform layer; the payload layouts are not
 * reversed, so the HAL only logs the callback identity for now. */
void oppoHwiNotifyCallback(long long mask, int algo, int status)
{
    LOGI("oppo hwi notify mask=0x%llx algo=%d status=%d", mask, algo, status);
}

void oppoHwiDataCallback(long long mask, const void *data, int size)
{
    LOGI("oppo hwi data mask=0x%llx data=%p size=%d", mask, data, size);
}

bool oppoHwiPropEnabled(const char *name)
{
    char prop[PROPERTY_VALUE_MAX] = {};
    property_get(name, prop, "0");
    return atoi(prop) == 1;
}

uint64_t oppoHwiPropMask()
{
    char prop[PROPERTY_VALUE_MAX] = {};
    property_get(kOppoHwiMaskProp, prop, "0x4080");
    errno = 0;
    char *end = nullptr;
    const unsigned long long parsed = strtoull(prop, &end, 0);
    if (end == prop || errno != 0) return kOppoHwiDefaultMask;
    return uint64_t(parsed) & ~kOppoHwiInProcessBackend;
}

void oppoHwiPutU32(uint8_t *base, size_t offset, uint32_t value)
{
    memcpy(base + offset, &value, sizeof(value));
}

void oppoHwiPutU8(uint8_t *base, size_t offset, uint8_t value)
{
    base[offset] = value;
}

int32_t oppoHwiPropInt(const char *name, int32_t defaultValue)
{
    char prop[PROPERTY_VALUE_MAX] = {};
    property_get(name, prop, "");
    if (prop[0] == '\0') return defaultValue;
    return atoi(prop);
}

bool oppoHwiSuperProbeEnabled()
{
    char prop[PROPERTY_VALUE_MAX] = {};
    property_get(kOppoHwiSuperProp, prop, "0");
    return atoi(prop) == 1;
}

/* Primary image of a frame: images[0] for the P3 multi-frame path, else legacy. */
bool oppoHwiPrimaryImage(const QCamera3OppoPlatformFrame &frame,
        QCamera3OppoPlatformImage &image)
{
    if (frame.imageCount > 0) {
        if (frame.imageCount > kOppoPlatformMaxImages) return false;
        image = frame.images[0];
    } else {
        image.data = frame.data;
        image.length = frame.length;
        image.width = frame.width;
        image.height = frame.height;
        image.stride = frame.stride;
        image.slice = frame.slice;
    }
    return image.data != nullptr && image.length != 0 &&
            image.width > 0 && image.height > 0;
}

bool oppoHwiPixelImage(const QCamera3OppoPlatformFrame &frame,
        qcamera::QCamera3HdrAlgo::Image &image)
{
    if (frame.imageCount != 0) return false;
    image.data = const_cast<uint8_t *>(frame.data);
    image.length = frame.length;
    image.width = frame.width;
    image.height = frame.height;
    image.yStride = image.uvStride = frame.stride;
    image.yScanline = frame.slice;
    image.uvScanline = frame.slice / 2;
    image.uvOffset = size_t(frame.stride) * frame.slice;
    return qcamera::QCamera3HdrAlgo::validImage(image);
}

bool oppoHwiPixelLibraryAvailable(uint64_t mask, bool realtime = false)
{
    const bool scene = mask == oppo_platform::kAlgoVdSces;
    const char *name = scene ? "libSCES.so" :
            (realtime ? "libVDRTImageAdjustment.so" : "libVDImageAdjustment.so");
    const char *sceneSymbols[] = {"Sces_initialize_gpu_engine", "Sces_process_snapshot",
            "Sces_release_gpu_engine"};
    const char *adjustSymbols[] = {"VDImageAdjustmentCreateAPI", "VDImageAdjustmentSetInputData",
            "VDImageAdjustmentApply", "VDImageAdjustmentUpdateResultBuffer",
            "VDImageAdjustmentReleaseAPI"};
    const char *realtimeSymbols[] = {"VDRTImageAdjustmentInitWithMode",
            "VDRTImageAdjustmentInit", "VDRTImageAdjustmentRelease",
            "VDRTImageAdjustmentProcess", "VDRTImageAdjustmentSetFilters",
            "VDRTImageAdjustmentSetVignetting", "VDRTImageAdjustmentSetGlobalFilterStrength",
            "VDRTImageAdjustmentResetOutput", "VDRTImageAdjustmentSetMaskParameters"};
    void *library = dlopen(name, RTLD_NOW | RTLD_LOCAL);
    if (!library) return false;
    const char **symbols = scene ? sceneSymbols : (realtime ? realtimeSymbols : adjustSymbols);
    const size_t count = scene ? 3 : (realtime ? 9 : 5);
    bool ready = true;
    for (size_t i = 0; i < count; ++i) ready = ready && dlsym(library, symbols[i]);
    dlclose(library);
    return ready;
}

bool oppoHwiPlatformLibraryAvailable()
{
    char path[PROPERTY_VALUE_MAX] = {};
    property_get(kOppoHwiPathProp, path, kOppoHwiDefaultPath);
    void *library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!library) return false;
    const bool ready = dlsym(library, "_Z18oppo_platform_initPvyPyS0_S0_") &&
            dlsym(library, "_Z21oppo_platform_processPvS_y") &&
            dlsym(library, "_Z20oppo_platform_uninitPvy");
    dlclose(library);
    return ready;
}

/* Property override wins (existence still required); otherwise first candidate
 * that exists on this device. Empty result means "do not register beauty". */
bool oppoHwiPathIsDirectory(const char *path)
{
    struct stat st = {};
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Property override wins (must be readable); otherwise the first existing
 * candidate. Reports the selected source tag and whether it is a directory so
 * the real-device log can tell file vs dir semantics apart. */
bool oppoHwiResolveModelPath(const char *propName, char *out, size_t outSize,
        const char **sourceOut, bool *dirOut)
{
    *sourceOut = "none";
    *dirOut = false;
    char prop[PROPERTY_VALUE_MAX] = {};
    property_get(propName, prop, "");
    if (prop[0] != '\0') {
        if (access(prop, R_OK) != 0) {
            LOGW("oppo hwi beauty model prop not readable: %s=%s", propName, prop);
            return false;
        }
        snprintf(out, outSize, "%s", prop);
        *sourceOut = "prop";
        *dirOut = oppoHwiPathIsDirectory(prop);
        return true;
    }
    for (size_t i = 0; i < sizeof(kOppoHwiBeautyModelCandidates) /
            sizeof(kOppoHwiBeautyModelCandidates[0]); ++i) {
        const OppoBeautyModelCandidate &candidate = kOppoHwiBeautyModelCandidates[i];
        if (access(candidate.path, R_OK) != 0) continue;
        snprintf(out, outSize, "%s", candidate.path);
        *sourceOut = candidate.source;
        *dirOut = oppoHwiPathIsDirectory(candidate.path) ||
                strstr(candidate.source, "-dir") != nullptr;
        return true;
    }
    return false;
}

}  // namespace

bool QCamera3OppoPlatformHwi::explicitAvailable(int32_t stillMode)
{
    if (stillMode == 5) return qcamera::QCamera3StockBeauty::isAvailable();
    if (stillMode == 10) return qcamera::QCamera3FrontBeauty::isAvailable();
    if (stillMode != 9) return false;
    return oppoHwiPixelLibraryAvailable(oppo_platform::kAlgoVdSces) &&
            oppoHwiPlatformLibraryAvailable();
}

bool QCamera3OppoPlatformHwi::imageAdjustAvailable(bool realtime)
{
    return oppoHwiPixelLibraryAvailable(oppo_platform::kAlgoImageAdjust, realtime) &&
            oppoHwiPlatformLibraryAvailable();
}

int32_t QCamera3OppoPlatformHwi::processExplicitFrame(int32_t stillMode,
        const QCamera3OppoPlatformFrame &frame, bool isFront)
{
    if (stillMode == 9)
        return processPixelStage(oppo_platform::kAlgoVdSces, frame, isFront, 0);
    qcamera::QCamera3HdrAlgo::Image image;
    if (!oppoHwiPixelImage(frame, image)) return -EINVAL;
    if (stillMode == 10 && isFront)
        return qcamera::QCamera3FrontBeauty::process(image, frame.beautyStrength);
    if (stillMode == 5 && !isFront) {
        if (frame.beautyStrength < 0 || frame.beautyStrength > 100) return -EINVAL;
        qcamera::QCamera3StockBeauty::Options options;
        options.soften = frame.beautyStrength;
        return qcamera::QCamera3StockBeauty::process(image, options);
    }
    return -EOPNOTSUPP;
}

int32_t QCamera3OppoPlatformHwi::processImageAdjustFrame(
        const QCamera3OppoPlatformFrame &frame, bool isFront, int32_t sensorSensitivity,
        bool realtime)
{
    if (sensorSensitivity <= 0) return -EINVAL;
    return processPixelStage(oppo_platform::kAlgoImageAdjust, frame, isFront,
            sensorSensitivity, realtime);
}

int32_t QCamera3OppoPlatformHwi::processPixelStage(uint64_t mask,
        const QCamera3OppoPlatformFrame &frame, bool isFront, int32_t sensorSensitivity,
        bool realtime)
{
    qcamera::QCamera3HdrAlgo::Image image;
    if (!oppoHwiPixelImage(frame, image)) return -EINVAL;
    if (frame.length > 64U * 1024U * 1024U) return -E2BIG;
    if (mask == oppo_platform::kAlgoVdSces &&
            (frame.sceneStrength < 0 || frame.sceneStrength > 100)) return -EINVAL;
    if (!oppoHwiPixelLibraryAvailable(mask, realtime)) return -EOPNOTSUPP;
    std::vector<uint8_t> pixels(frame.data, frame.data + frame.length);
    QCamera3OppoPlatformFrame working = frame;
    working.data = pixels.data();
    pthread_mutex_lock(&mLock);
    shutdownLocked();
    if (mUnavailable) {
        if (mLib) dlclose(mLib);
        mLib = nullptr;
        mUnavailable = false;
    }
    mMask = mask;
    mIsFront = isFront;
    int32_t rc = -ENODEV;
    // SCES uses stream_type bit0 to initialize its GPU, even for snapshots.
    const bool initializeRealtime = realtime || mask == oppo_platform::kAlgoVdSces;
    if (initializeLocked(&working, initializeRealtime)) {
        oppo_platform::Frame params;
        params.data = pixels.data();
        params.length = pixels.size();
        params.width = frame.width;
        params.height = frame.height;
        params.stride = frame.stride;
        params.slice = frame.slice;
        params.isFront = isFront;
        params.scesParam = frame.sceneStrength;
        params.imageAdjust = mask == oppo_platform::kAlgoImageAdjust;
        params.realtime = realtime;
        params.sensorSensitivity = sensorSensitivity;
        params.scratchFd = mScratchFd;
        params.scratchPtr = mScratch;
        params.scratchSize = mScratchSize;
        if (!oppo_platform::buildParam(mParam, mParamSize, params) ||
                !oppo_platform::buildFrameCtx(mFrameCtx, mFrameCtxSize, params)) {
            rc = -EINVAL;
        } else {
            typedef int32_t (*ProcessFn)(void *, void *, uint64_t);
            rc = reinterpret_cast<ProcessFn>(mProcess)(mFrameCtx, mParam,
                    mask | oppo_platform::kInProcessBackend);
            if (rc > 0) rc = -EIO;
            // SCES and the VD snapshot wrapper can swallow their library errors.
            // An unchanged output must be reported as bypassed, never applied.
            if (rc == 0) {
                bool changed = false;
                for (int row = 0; row < frame.height && !changed; ++row)
                    changed = memcmp(frame.data + size_t(row) * image.yStride,
                            pixels.data() + size_t(row) * image.yStride, image.width) != 0;
                for (int row = 0; row < frame.height / 2 && !changed; ++row)
                    changed = memcmp(frame.data + image.uvOffset + size_t(row) * image.uvStride,
                            pixels.data() + image.uvOffset + size_t(row) * image.uvStride,
                            image.width) != 0;
                if (!changed) rc = -ENODATA;
            }
        }
        shutdownLocked();
    }
    mLastStatus = rc;
    pthread_mutex_unlock(&mLock);
    if (rc == 0) {
        for (int row = 0; row < frame.height; ++row)
            memcpy(image.data + size_t(row) * image.yStride,
                    pixels.data() + size_t(row) * image.yStride, image.width);
        for (int row = 0; row < frame.height / 2; ++row)
            memcpy(image.data + image.uvOffset + size_t(row) * image.uvStride,
                    pixels.data() + image.uvOffset + size_t(row) * image.uvStride, image.width);
    }
    return rc;
}

bool QCamera3OppoPlatformHwi::configured()
{
    return oppoHwiPropEnabled(kOppoHwiProbeProp) ||
            oppoHwiPropEnabled(kOppoHwiProcessProp);
}

bool QCamera3OppoPlatformHwi::runEnabled()
{
    return oppoHwiPropEnabled(kOppoHwiRunProp);
}

uint64_t QCamera3OppoPlatformHwi::sessionAlgorithmMask()
{
    /* Only the modes wired for HAL3; HDR/LLHDR/MFNR/bokeh stay out (RE negative
     * evidence: HDR/LLHDR bits are unreachable, MFNR has no platform bit, bokeh
     * needs the unproven dual-image/output contract). SUPER is registered only
     * for the property-gated single-frame probe. */
    using namespace qcamera::oppo_platform;
    uint64_t mask = kSupportedPlatformMask & ~kAlgoSuperPhoto;
    if (oppoHwiSuperProbeEnabled()) mask |= kAlgoSuperPhoto;
    return mask;
}

int32_t QCamera3OppoPlatformHwi::lastStatus() const
{
    return mLastStatus;
}

bool QCamera3OppoPlatformHwi::beautyParamsReady(int32_t stillMode) const
{
    if (stillMode == 5) return mBeautyModelRear[0] != '\0';
    if (stillMode == 10) return mBeautyModelFront[0] != '\0';
    return true;
}

bool QCamera3OppoPlatformHwi::superProbeReady() const
{
    return oppoHwiSuperProbeEnabled() && oppoHwiPropEnabled(kOppoHwiUpdateProp);
}

void QCamera3OppoPlatformHwi::resolveBeautyParamsLocked()
{
    memset(mBeautyModelRear, 0, sizeof(mBeautyModelRear));
    memset(mBeautyModelFront, 0, sizeof(mBeautyModelFront));
    const char *rearSource = "none";
    const char *frontSource = "none";
    bool rearDir = false;
    bool frontDir = false;
    const bool rear = oppoHwiResolveModelPath(kOppoHwiBeautyModelProp,
            mBeautyModelRear, sizeof(mBeautyModelRear), &rearSource, &rearDir);
    const bool front = oppoHwiResolveModelPath(kOppoHwiBeautyModelFrontProp,
            mBeautyModelFront, sizeof(mBeautyModelFront), &frontSource, &frontDir);
    mBeautyParam24 = oppoHwiPropInt(kOppoHwiBeautyParam24Prop, 0);
    mBeautyThreshold1 = oppoHwiPropInt("persist.vendor.camera.oppo.platform_hwi.beauty_threshold1", 0);
    mBeautyThreshold2 = oppoHwiPropInt("persist.vendor.camera.oppo.platform_hwi.beauty_threshold2", 0);
    mBeautyMode = oppoHwiPropInt(kOppoHwiBeautyModeProp, 0);
    mBeautyEnable = oppoHwiPropInt(kOppoHwiBeautyEnableProp, 0);
    mFaceFdSelect = oppoHwiPropInt(kOppoHwiFaceFdSelectProp, 0);
    mFaceInfoMode = oppoHwiPropInt(kOppoHwiFaceInfoModeProp, 0);
    char scesProp[PROPERTY_VALUE_MAX] = {};
    property_get(kOppoHwiScesParamProp, scesProp, "");
    mScesParamOverrideValid = scesProp[0] != '\0';
    mScesParam = mScesParamOverrideValid ? atoi(scesProp) : 0;
    char rearSuffix[8] = "";
    char frontSuffix[8] = "";
    if (rearDir && strstr(rearSource, "-dir") == nullptr) snprintf(rearSuffix, sizeof(rearSuffix), ",dir");
    if (frontDir && strstr(frontSource, "-dir") == nullptr) snprintf(frontSuffix, sizeof(frontSuffix), ",dir");
    LOGI("oppo hwi beauty model rear=%s (source=%s%s) front=%s (source=%s%s)",
            rear ? mBeautyModelRear : "(none)", rearSource, rearSuffix,
            front ? mBeautyModelFront : "(none)", frontSource, frontSuffix);
}

void QCamera3OppoPlatformHwi::setFaceInfo(uint32_t frameNumber,
        const QCamera3OppoPlatformFaceInfo &info)
{
    pthread_mutex_lock(&sOppoFaceLock);
    OppoFaceCacheEntry *slot = nullptr;
    for (size_t i = 0; i < kOppoFaceCacheSlots; ++i) {
        if (sOppoFaceCache[i].valid && sOppoFaceCache[i].frame == frameNumber) {
            slot = &sOppoFaceCache[i];
            break;
        }
    }
    if (slot == nullptr) {
        slot = &sOppoFaceCache[sOppoFaceNext];
        sOppoFaceNext = (sOppoFaceNext + 1) % kOppoFaceCacheSlots;
    }
    slot->frame = frameNumber;
    slot->valid = true;
    slot->info = info;
    pthread_mutex_unlock(&sOppoFaceLock);
}

bool QCamera3OppoPlatformHwi::getFaceInfo(uint32_t frameNumber,
        QCamera3OppoPlatformFaceInfo &info)
{
    pthread_mutex_lock(&sOppoFaceLock);
    for (size_t i = 0; i < kOppoFaceCacheSlots; ++i) {
        const OppoFaceCacheEntry &slot = sOppoFaceCache[i];
        if (slot.valid && slot.frame == frameNumber) {
            info = slot.info;
            pthread_mutex_unlock(&sOppoFaceLock);
            return true;
        }
    }
    /* The snapshot request usually has a face result from a slightly older
     * preview frame; only reuse it within a bounded age, never stale forever. */
    const OppoFaceCacheEntry *best = nullptr;
    for (size_t i = 0; i < kOppoFaceCacheSlots; ++i) {
        const OppoFaceCacheEntry &slot = sOppoFaceCache[i];
        if (!slot.valid || slot.frame > frameNumber) continue;
        if ((frameNumber - slot.frame) > kOppoFaceMaxAgeFrames) continue;
        if (best == nullptr || slot.frame > best->frame) best = &slot;
    }
    if (best != nullptr) {
        info = best->info;
        pthread_mutex_unlock(&sOppoFaceLock);
        return true;
    }
    info = QCamera3OppoPlatformFaceInfo();
    pthread_mutex_unlock(&sOppoFaceLock);
    return false;
}

QCamera3OppoPlatformHwi &QCamera3OppoPlatformHwi::get()
{
    static QCamera3OppoPlatformHwi instance;
    sOppoHwiCreated = true;
    return instance;
}

void QCamera3OppoPlatformHwi::shutdownOnClose()
{
    if (!sOppoHwiCreated) return;
    get().onCloseCamera();
}

QCamera3OppoPlatformHwi::QCamera3OppoPlatformHwi() :
        mLib(nullptr),
        mInit(nullptr),
        mProcess(nullptr),
        mUninit(nullptr),
        mUpdateParameters(nullptr),
        mParam(nullptr),
        mFrameCtx(nullptr),
        mParamSize(kOppoHwiParamSize),
        mFrameCtxSize(kOppoHwiFrameCtxSize),
        mMask(kOppoHwiDefaultMask),
        mCameraId(-1),
        mIsFront(false),
        mInitialized(false),
        mUnavailable(false),
        mFrameCount(0)
{
    pthread_mutex_init(&mLock, nullptr);
}

QCamera3OppoPlatformHwi::~QCamera3OppoPlatformHwi()
{
    onCloseCamera();
    freeScratchLocked();
    if (mParam != nullptr) free(mParam);
    if (mFrameCtx != nullptr) free(mFrameCtx);
    if (mLib != nullptr) dlclose(mLib);
    pthread_mutex_destroy(&mLock);
}

bool QCamera3OppoPlatformHwi::ensureScratchLocked()
{
    if (mScratch != nullptr) return true;
    void *scratch = nullptr;
    if (posix_memalign(&scratch, kOppoHwiScratchAlign, kOppoHwiScratchSize) != 0 ||
            scratch == nullptr) {
        LOGE("oppo hwi scratch allocation failed");
        return false;
    }
    memset(scratch, 0, kOppoHwiScratchSize);
    mScratch = scratch;
    mScratchSize = kOppoHwiScratchSize;
    mScratchFd = oppoHwiCreateScratchFd(mScratchSize);
    LOGI("oppo hwi scratch ptr=%p fd=%d size=%zu", mScratch, mScratchFd, mScratchSize);
    return true;
}

void QCamera3OppoPlatformHwi::freeScratchLocked()
{
    if (mScratchFd >= 0) {
        close(mScratchFd);
        mScratchFd = -1;
    }
    free(mScratch);
    mScratch = nullptr;
}

void QCamera3OppoPlatformHwi::onOpenCamera(int32_t cameraId, bool isFront)
{
    pthread_mutex_lock(&mLock);
    mCameraId = cameraId;
    mIsFront = isFront;
    mMask = runEnabled() ? sessionAlgorithmMask() : oppoHwiPropMask();
    mLastStatus = -EOPNOTSUPP;
    resolveBeautyParamsLocked();
    if (!runEnabled() && oppoHwiPropEnabled(kOppoHwiProbeProp)) {
        /* Stage 1 probe: no stream exists yet, so p1 carries zero geometry.
         * The explicit-mode run path initializes on the first real frame instead. */
        if (initializeLocked(nullptr)) shutdownLocked();
    }
    pthread_mutex_unlock(&mLock);
}

void QCamera3OppoPlatformHwi::onCloseCamera()
{
    pthread_mutex_lock(&mLock);
    shutdownLocked();
    pthread_mutex_unlock(&mLock);
}

bool QCamera3OppoPlatformHwi::initializeLocked(
        const QCamera3OppoPlatformFrame *frame, bool realtime)
{
    if (mInitialized) return true;
    if (mUnavailable) return false;

    if (mLib == nullptr) {
        char path[PROPERTY_VALUE_MAX] = {};
        property_get(kOppoHwiPathProp, path, kOppoHwiDefaultPath);
        const char *const libPath = path[0] != '\0' ? path : kOppoHwiDefaultPath;
        mLib = dlopen(libPath, RTLD_NOW | RTLD_LOCAL);
        if (mLib == nullptr) {
            LOGE("oppo hwi dlopen(%s) failed: %s", libPath, dlerror());
            mUnavailable = true;
            return false;
        }
        LOGI("oppo hwi dlopen(%s) ok", libPath);
        /* C++-mangled entry points; connect() is not resolved. */
        mInit = dlsym(mLib, "_Z18oppo_platform_initPvyPyS0_S0_");
        mProcess = dlsym(mLib, "_Z21oppo_platform_processPvS_y");
        mUninit = dlsym(mLib, "_Z20oppo_platform_uninitPvy");
        mUpdateParameters = dlsym(mLib, "_Z30oppo_platform_updateparametersPvS_y");
        mSetCallbacks = dlsym(mLib, "oppo_platform_set_callbacks");
        if (mInit == nullptr || mProcess == nullptr || mUninit == nullptr) {
            LOGE("oppo hwi dlsym failed: init=%p process=%p uninit=%p",
                    mInit, mProcess, mUninit);
            mUnavailable = true;
            return false;
        }
        /* updateparameters() is resolved but not called: its blob layout is unknown. */
        LOGI("oppo hwi symbols: init=%p process=%p uninit=%p update=%p callbacks=%p",
                mInit, mProcess, mUninit, mUpdateParameters, mSetCallbacks);
    }

    if (mParam == nullptr || mFrameCtx == nullptr) {
        free(mParam);
        mParam = nullptr;
        free(mFrameCtx);
        mFrameCtx = nullptr;
        mParam = static_cast<uint8_t *>(calloc(1, mParamSize));
        mFrameCtx = static_cast<uint8_t *>(calloc(1, mFrameCtxSize));
        if (mParam == nullptr || mFrameCtx == nullptr) {
            LOGE("oppo hwi allocation failed (param=%p ctx=%p)", mParam, mFrameCtx);
            mUnavailable = true;
            return false;
        }
    }
    if (!ensureScratchLocked()) {
        mUnavailable = true;
        return false;
    }

    /* Stage 1 passes NULL, stage 2 the first frame with real geometry. */
    buildPlatformParamLocked(frame);
    oppo_platform::writeU32(mParam, oppo_platform::ParamOffsets::kStreamType,
            realtime ? 1 : 0);
    /* out1 = offline mask, out2 = single-frame/preview mask, out3 =
     * multi-frame/paired mask (platform_hwi init disassembly, bits 0x1ffb8 /
     * 0x10047); they are diagnostics here: the caller still passes the mask. */
    uint64_t offlineMask = 0;
    uint64_t frameMask = 0;
    uint64_t multiframeMask = 0;
    typedef int32_t (*InitFn)(void *, uint64_t, uint64_t *, uint64_t *, uint64_t *);
    const int32_t rc = reinterpret_cast<InitFn>(mInit)(mParam,
            mMask | kOppoHwiInProcessBackend, &offlineMask, &frameMask,
            &multiframeMask);
    LOGI("oppo hwi init(%s) cam=%d mask=0x%016" PRIx64 " rc=%d "
            "offlineMask=0x%016" PRIx64 " frameMask=0x%016" PRIx64
            " multiFrameMask=0x%016" PRIx64,
            frame != nullptr ? "first-frame" : "probe", mCameraId,
            mMask | kOppoHwiInProcessBackend, rc, offlineMask, frameMask,
            multiframeMask);
    if (rc != 0) {
        mUnavailable = true;
        return false;
    }
    mInitialized = true;
    if (oppoHwiPropEnabled(kOppoHwiCallbackProp) && mSetCallbacks != nullptr) {
        typedef void (*NotifyFn)(long long, int, int);
        typedef void (*DataFn)(long long, const void *, int);
        typedef int32_t (*SetCallbacksFn)(NotifyFn, DataFn);
        const int32_t cbRc = reinterpret_cast<SetCallbacksFn>(mSetCallbacks)(
                oppoHwiNotifyCallback, oppoHwiDataCallback);
        LOGI("oppo hwi set_callbacks rc=%d", cbRc);
    }
    return true;
}

void QCamera3OppoPlatformHwi::shutdownLocked()
{
    if (mInitialized && mUninit != nullptr && mParam != nullptr) {
        typedef int32_t (*UninitFn)(void *, uint64_t);
        const int32_t rc = reinterpret_cast<UninitFn>(mUninit)(mParam,
                mMask | kOppoHwiInProcessBackend);
        LOGI("oppo hwi uninit cam=%d mask=0x%016" PRIx64 " rc=%d", mCameraId,
                mMask | kOppoHwiInProcessBackend, rc);
    }
    mInitialized = false;
}

/* Fills the geometry and is-front fields; the stock HAL1-only fields stay 0. */
void QCamera3OppoPlatformHwi::buildPlatformParamLocked(
        const QCamera3OppoPlatformFrame *frame)
{
    if (mParam == nullptr) return;
    memset(mParam, 0, mParamSize);
    const uint32_t width = frame != nullptr && frame->width > 0 ?
            uint32_t(frame->width) : 0;
    const uint32_t height = frame != nullptr && frame->height > 0 ?
            uint32_t(frame->height) : 0;
    const uint32_t stride = frame != nullptr && frame->stride > 0 ?
            uint32_t(frame->stride) : width;
    const uint32_t slice = frame != nullptr && frame->slice > 0 ?
            uint32_t(frame->slice) : height;
    oppoHwiPutU32(mParam, kParamWidth, width);
    oppoHwiPutU32(mParam, kParamHeight, height);
    oppoHwiPutU32(mParam, kParamStride, stride);
    oppoHwiPutU32(mParam, kParamSlice, slice);
    oppoHwiPutU32(mParam, kParamFlipMode, 0);
    oppoHwiPutU32(mParam, kParamVisiontechFilterType, 0);
    oppoHwiPutU8(mParam, kParamIsFront, mIsFront ? 1 : 0);
    oppoHwiPutU8(mParam, kParamOppoCapModeB, 0);
    oppoHwiPutU8(mParam, kParamVendorCustomApi, 0);
    oppoHwiPutU32(mParam, kParamPortraitLightMode, 0);
    oppoHwiPutU32(mParam, kParamJpegRotation, 0);
    oppoHwiPutU32(mParam, kParamOarNeedStop, 0);
}

/* Full minimum safe set: image slot 0 (ptr/geometry/format/valid), image count
 * and the session scratch descriptor; the unknown tail stays zero. */
void QCamera3OppoPlatformHwi::buildFrameCtxLocked(
        const QCamera3OppoPlatformFrame &frame)
{
    if (mFrameCtx == nullptr) return;
    oppo_platform::Frame params;
    params.data = frame.data;
    params.length = frame.length;
    params.width = frame.width;
    params.height = frame.height;
    params.stride = frame.stride;
    params.slice = frame.slice;
    params.scratchFd = mScratchFd;
    params.scratchPtr = mScratch;
    params.scratchSize = mScratchSize;
    int32_t count = frame.imageCount;
    if (count < 0) count = 0;
    if (count > oppo_platform::kMaxFrameImages) count = oppo_platform::kMaxFrameImages;
    params.imageCount = count;
    for (int32_t i = 0; i < count; ++i) {
        params.images[i].data = frame.images[i].data;
        params.images[i].length = frame.images[i].length;
        params.images[i].width = frame.images[i].width;
        params.images[i].height = frame.images[i].height;
        params.images[i].stride = frame.images[i].stride;
        params.images[i].slice = frame.images[i].slice;
    }
    oppo_platform::buildFrameCtx(mFrameCtx, mFrameCtxSize, params);
}

void QCamera3OppoPlatformHwi::dumpFrameLocked(
        const QCamera3OppoPlatformFrame &frame, const char *tag)
{
    char path[128] = {};
    snprintf(path, sizeof(path), "%s/oppo_hwi_%u_%s.nv21", kOppoHwiDumpDir,
            mFrameCount, tag);
    FILE *file = fopen(path, "wb");
    if (file == nullptr) {
        LOGE("oppo hwi dump %s failed: %s", path, strerror(errno));
        return;
    }
    QCamera3OppoPlatformImage primary;
    if (!oppoHwiPrimaryImage(frame, primary)) {
        fclose(file);
        return;
    }
    const size_t written = fwrite(primary.data, 1, primary.length, file);
    fclose(file);
    LOGI("oppo hwi dump %s bytes=%zu/%zu", path, written, primary.length);
}

namespace {
int32_t oppoHwiCallProcess(void *processFn, void *frameCtx, void *param, uint64_t mask)
{
    typedef int32_t (*ProcessFn)(void *, void *, uint64_t);
    return reinterpret_cast<ProcessFn>(processFn)(frameCtx, param, mask);
}
}  // namespace

int32_t QCamera3OppoPlatformHwi::processStillFrame(int32_t stillMode,
        const QCamera3OppoPlatformFrame &frame)
{
    if (!runEnabled()) {
        mLastStatus = -EOPNOTSUPP;
        return mLastStatus;
    }
    /* RE task-74: beauty needs an existing model path; never run with an empty
     * path ("success but no effect"). Fail closed so callers fall back. */
    if ((stillMode == 5 || stillMode == 10) && !beautyParamsReady(stillMode)) {
        mLastStatus = -EOPNOTSUPP;
        LOGW("oppo hwi: beauty model path unavailable; stillMode %d not run", stillMode);
        return mLastStatus;
    }
    /* SUPER(4) is a property-gated single-frame probe only: the original
     * superphoto updateparameters/process contract is not fully reversed. */
    if (stillMode == 4) {
        if (!oppoHwiSuperProbeEnabled()) {
            mLastStatus = -EOPNOTSUPP;
            LOGW("oppo hwi: SUPER probe disabled (platform_hwi.super=0)");
            return mLastStatus;
        }
        if (!oppoHwiPropEnabled(kOppoHwiUpdateProp)) {
            mLastStatus = -EOPNOTSUPP;
            LOGW("oppo hwi: SUPER probe needs platform_hwi.update=1");
            return mLastStatus;
        }
    }
    const uint64_t modeMask = oppo_platform::maskForStillMode(stillMode);
    if (!oppo_platform::isPlatformStillMode(stillMode) ||
            !oppo_platform::supportedMask(modeMask)) {
        /* Only SCES(9)/front beauty(10)/rear beauty(5) are wired. */
        LOGE("oppo hwi: stillMode %d is not wired to the platform block", stillMode);
        mLastStatus = -EOPNOTSUPP;
        return mLastStatus;
    }
    QCamera3OppoPlatformImage primary;
    if (!oppoHwiPrimaryImage(frame, primary)) {
        mLastStatus = -EINVAL;
        return mLastStatus;
    }

    pthread_mutex_lock(&mLock);
    if (!initializeLocked(&frame)) {
        mLastStatus = mUnavailable ? -ENODEV : -EIO;
        pthread_mutex_unlock(&mLock);
        return mLastStatus;
    }
    if (mScratch == nullptr || mScratchSize < oppo_platform::kScratchBytes) {
        /* Algorithms read ctx+0x2cc without a null check; never run them blind. */
        LOGE("oppo hwi scratch unavailable; refusing platform path");
        mLastStatus = -ENOMEM;
        pthread_mutex_unlock(&mLock);
        return mLastStatus;
    }
    oppo_platform::Frame params;
    params.data = primary.data;
    params.length = primary.length;
    params.width = primary.width;
    params.height = primary.height;
    params.stride = primary.stride > 0 ? primary.stride : primary.width;
    params.slice = primary.slice > 0 ? primary.slice : primary.height;
    int32_t imageCount = frame.imageCount;
    if (imageCount < 0) imageCount = 0;
    if (imageCount > oppo_platform::kMaxFrameImages) {
        imageCount = oppo_platform::kMaxFrameImages;
    }
    params.imageCount = imageCount;
    for (int32_t i = 0; i < imageCount; ++i) {
        params.images[i].data = frame.images[i].data;
        params.images[i].length = frame.images[i].length;
        params.images[i].width = frame.images[i].width;
        params.images[i].height = frame.images[i].height;
        params.images[i].stride = frame.images[i].stride;
        params.images[i].slice = frame.images[i].slice;
    }
    params.isFront = mIsFront ? 1 : 0;
    params.jpegRotation = 0;
    params.scratchFd = mScratchFd;
    params.scratchPtr = mScratch;
    params.scratchSize = mScratchSize;
    int32_t faceCount = frame.faceInfo.count;
    if (faceCount < 0) faceCount = 0;
    if (faceCount > kOppoPlatformMaxFaces) faceCount = kOppoPlatformMaxFaces;
    params.faceInfo.count = faceCount;
    params.faceInfo.sourceWidth = frame.faceInfo.sourceWidth;
    params.faceInfo.sourceHeight = frame.faceInfo.sourceHeight;
    for (int32_t i = 0; i < faceCount; ++i) {
        params.faceInfo.faces[i].left = frame.faceInfo.faces[i].left;
        params.faceInfo.faces[i].top = frame.faceInfo.faces[i].top;
        params.faceInfo.faces[i].width = frame.faceInfo.faces[i].width;
        params.faceInfo.faces[i].height = frame.faceInfo.faces[i].height;
    }
    if (stillMode == 5 || stillMode == 10) {
        params.beauty.enabled = true;
        params.beauty.modelPath = stillMode == 10 ? mBeautyModelFront : mBeautyModelRear;
        params.beauty.strength = frame.beautyStrength;
        params.beauty.param24 = mBeautyParam24;
        params.beauty.threshold1 = mBeautyThreshold1;
        params.beauty.threshold2 = mBeautyThreshold2;
        params.beauty.mode = mBeautyMode;
        params.beauty.enable = mBeautyEnable;
        params.beauty.fdSelect = mFaceFdSelect;
        params.beauty.faceInfoMode = mFaceInfoMode;
    }
    if (stillMode == 9) {
        int32_t strength = frame.sceneStrength;
        if (strength < 0) strength = 0;
        if (strength > 100) strength = 100;
        params.scesParam = mScesParamOverrideValid ? mScesParam :
                (strength > 0 ? strength : 0);
        LOGI("oppo hwi sces strength=%d param=%u", frame.sceneStrength,
                params.scesParam);
    }
    if (!oppo_platform::buildParam(mParam, mParamSize, params) ||
            !oppo_platform::buildFrameCtx(mFrameCtx, mFrameCtxSize, params)) {
        mLastStatus = -EINVAL;
        pthread_mutex_unlock(&mLock);
        return mLastStatus;
    }
    const uint64_t callMask = modeMask | oppo_platform::kInProcessBackend;
    /* process() filters again against the mask registered by init(): a mode bit
     * outside the session mask would be silently skipped. */
    if ((modeMask & ~sessionAlgorithmMask()) != 0) {
        LOGE("oppo hwi: stillMode %d mask not registered at init", stillMode);
        mLastStatus = -EINVAL;
        pthread_mutex_unlock(&mLock);
        return mLastStatus;
    }
    /* updateparameters() takes the same param_platform_t and only the
     * superphoto/bokeh entries implement it; keep it behind the A/B gate. */
    if (oppoHwiPropEnabled(kOppoHwiUpdateProp) &&
            (modeMask & (oppo_platform::kAlgoSuperPhoto | oppo_platform::kAlgoBokeh)) != 0) {
        if (mUpdateParameters != nullptr) {
            typedef int32_t (*UpdateFn)(void *, void *, uint64_t);
            const int32_t updateRc = reinterpret_cast<UpdateFn>(mUpdateParameters)(
                    mParam, mFrameCtx, callMask);
            LOGI("oppo hwi updateparameters stillMode=%d mask=0x%016" PRIx64 " rc=%d",
                    stillMode, callMask, updateRc);
        } else {
            LOGW("oppo hwi updateparameters requested but symbol is missing");
        }
    }
    ++mFrameCount;
    const bool dump = oppoHwiPropEnabled(kOppoHwiDumpProp);
    if (stillMode == 4 && dump) dumpFrameLocked(frame, "super_pre");
    struct timespec begin = {};
    struct timespec end = {};
    clock_gettime(CLOCK_MONOTONIC, &begin);
    const int32_t rc = oppoHwiCallProcess(mProcess, mFrameCtx, mParam, callMask);
    clock_gettime(CLOCK_MONOTONIC, &end);
    const int64_t elapsedUs = (int64_t(end.tv_sec) - int64_t(begin.tv_sec)) * 1000000 +
            (int64_t(end.tv_nsec) - int64_t(begin.tv_nsec)) / 1000;
    mLastStatus = rc == 0 ? 0 : (rc < 0 ? rc : -EIO);
    if (stillMode == 4 && dump) dumpFrameLocked(frame, "super_post");
    LOGI("oppo hwi processStillFrame stillMode=%d algo=%s mask=0x%016" PRIx64
            " cam=%d size=%dx%d images=%d rc=%d elapsed=%" PRId64 "us",
            stillMode, oppo_platform::algorithmName(modeMask & ~oppo_platform::kInProcessBackend),
            callMask, mCameraId, primary.width, primary.height, imageCount, rc, elapsedUs);
    pthread_mutex_unlock(&mLock);
    return mLastStatus;
}

bool QCamera3OppoPlatformHwi::processFrame(const QCamera3OppoPlatformFrame &frame)
{
    if (!oppoHwiPropEnabled(kOppoHwiProcessProp)) return false;
    if (frame.data == nullptr || frame.length == 0 ||
            frame.width <= 0 || frame.height <= 0) return false;

    pthread_mutex_lock(&mLock);
    /* Register on the first frame with real geometry, then process that same frame. */
    if (!initializeLocked(&frame)) {
        pthread_mutex_unlock(&mLock);
        return false;
    }
    buildPlatformParamLocked(&frame);
    buildFrameCtxLocked(frame);
    ++mFrameCount;
    if (oppoHwiPropEnabled(kOppoHwiDumpProp)) dumpFrameLocked(frame, "pre");

    /* The layer rewrites the frame bytes in place, so a failure cannot be rolled back. */
    typedef int32_t (*ProcessFn)(void *, void *, uint64_t);
    struct timespec begin = {};
    struct timespec end = {};
    clock_gettime(CLOCK_MONOTONIC, &begin);
    const int32_t rc = reinterpret_cast<ProcessFn>(mProcess)(mFrameCtx, mParam,
            mMask | kOppoHwiInProcessBackend);
    clock_gettime(CLOCK_MONOTONIC, &end);
    const int64_t elapsedUs = (int64_t(end.tv_sec) - int64_t(begin.tv_sec)) * 1000000 +
            (int64_t(end.tv_nsec) - int64_t(begin.tv_nsec)) / 1000;
    LOGI("oppo hwi process cam=%d frame=%u size=%dx%d stride=%d mask=0x%016" PRIx64
            " rc=%d elapsed=%" PRId64 "us",
            mCameraId, mFrameCount, frame.width, frame.height, frame.stride,
            mMask | kOppoHwiInProcessBackend, rc, elapsedUs);
    if (oppoHwiPropEnabled(kOppoHwiDumpProp)) dumpFrameLocked(frame, "post");

    pthread_mutex_unlock(&mLock);
    return rc == 0;
}
