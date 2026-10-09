/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3OppoPlatformHwi.h"

/* mm_camera_dbg.h lacks its own extern "C" block; the symbol has C linkage. */
extern "C" {
#include "mm_camera_dbg.h"
}

#include <cutils/properties.h>
#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* In-process wiring of the OPPO platform algorithm layer. The layer keeps one
 * .bss singleton per algorithm, so one process-wide mutex covers every call. */

namespace {

const char *const kOppoHwiProbeProp = "persist.vendor.camera.oppo.platform_hwi";
const char *const kOppoHwiProcessProp =
        "persist.vendor.camera.oppo.platform_hwi.process";
const char *const kOppoHwiMaskProp = "persist.vendor.camera.oppo.platform_hwi.mask";
const char *const kOppoHwiPathProp = "persist.vendor.camera.oppo.platform_hwi.path";
const char *const kOppoHwiDumpProp = "persist.vendor.camera.oppo.platform_hwi.dump";
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

/* Frame ctx offsets. */
const size_t kFrameCtxWidth = 0x10;
const size_t kFrameCtxHeight = 0x14;
const size_t kFrameCtxStride = 0x18;
const size_t kFrameCtxSlice = 0x1c;

bool sOppoHwiCreated = false;

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

}  // namespace

bool QCamera3OppoPlatformHwi::configured()
{
    return oppoHwiPropEnabled(kOppoHwiProbeProp) ||
            oppoHwiPropEnabled(kOppoHwiProcessProp);
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
    if (mParam != nullptr) free(mParam);
    if (mFrameCtx != nullptr) free(mFrameCtx);
    if (mLib != nullptr) dlclose(mLib);
    pthread_mutex_destroy(&mLock);
}

void QCamera3OppoPlatformHwi::onOpenCamera(int32_t cameraId, bool isFront)
{
    pthread_mutex_lock(&mLock);
    mCameraId = cameraId;
    mIsFront = isFront;
    mMask = oppoHwiPropMask();
    if (oppoHwiPropEnabled(kOppoHwiProbeProp)) {
        /* Stage 1 probe: no stream exists yet, so p1 carries zero geometry. */
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
        const QCamera3OppoPlatformFrame *frame)
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
        if (mInit == nullptr || mProcess == nullptr || mUninit == nullptr) {
            LOGE("oppo hwi dlsym failed: init=%p process=%p uninit=%p",
                    mInit, mProcess, mUninit);
            mUnavailable = true;
            return false;
        }
        /* updateparameters() is resolved but not called: its blob layout is unknown. */
        LOGI("oppo hwi symbols: init=%p process=%p uninit=%p update=%p (update unused)",
                mInit, mProcess, mUninit, mUpdateParameters);
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

    /* Stage 1 passes NULL, stage 2 the first frame with real geometry. */
    buildPlatformParamLocked(frame);
    uint64_t outOffline = 0;
    uint64_t outRegistered = 0;
    uint64_t outPreview = 0;
    typedef int32_t (*InitFn)(void *, uint64_t, uint64_t *, uint64_t *, uint64_t *);
    const int32_t rc = reinterpret_cast<InitFn>(mInit)(mParam,
            mMask | kOppoHwiInProcessBackend, &outOffline, &outRegistered,
            &outPreview);
    LOGI("oppo hwi init(%s) cam=%d mask=0x%016" PRIx64 " rc=%d "
            "outOffline=0x%016" PRIx64 " outRegistered=0x%016" PRIx64
            " outPreview=0x%016" PRIx64,
            frame != nullptr ? "first-frame" : "probe", mCameraId,
            mMask | kOppoHwiInProcessBackend, rc, outOffline, outRegistered,
            outPreview);
    if (rc != 0) {
        mUnavailable = true;
        return false;
    }
    mInitialized = true;
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

/* Only width/height/stride/slice are filled; the rest stays 0 like the stock memset. */
void QCamera3OppoPlatformHwi::buildFrameCtxLocked(
        const QCamera3OppoPlatformFrame &frame)
{
    if (mFrameCtx == nullptr) return;
    memset(mFrameCtx, 0, mFrameCtxSize);
    oppoHwiPutU32(mFrameCtx, kFrameCtxWidth, uint32_t(frame.width));
    oppoHwiPutU32(mFrameCtx, kFrameCtxHeight, uint32_t(frame.height));
    oppoHwiPutU32(mFrameCtx, kFrameCtxStride, uint32_t(frame.stride));
    oppoHwiPutU32(mFrameCtx, kFrameCtxSlice, uint32_t(frame.slice));
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
    const size_t written = fwrite(frame.data, 1, frame.length, file);
    fclose(file);
    LOGI("oppo hwi dump %s bytes=%zu/%zu", path, written, frame.length);
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
