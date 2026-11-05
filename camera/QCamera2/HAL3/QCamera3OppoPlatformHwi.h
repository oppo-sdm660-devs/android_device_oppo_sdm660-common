/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_OPPO_PLATFORM_HWI_H
#define QCAMERA3_OPPO_PLATFORM_HWI_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <errno.h>

/*
 * In-process wiring of the OPPO platform algorithm layer (liboppo_platform_hwi.so).
 * Off by default: the persist.vendor.camera.oppo.platform_hwi[.process] gates enable it.
 */

constexpr int32_t kOppoPlatformMaxFaces = 10;

/* One face rect in the coordinate space given by sourceWidth/Height (HAL3 active
 * array); the platform param builder scales it to the image geometry. */
struct QCamera3OppoPlatformFaceRect {
    int32_t left = 0;
    int32_t top = 0;
    int32_t width = 0;
    int32_t height = 0;
};

struct QCamera3OppoPlatformFaceInfo {
    int32_t count = 0;
    int32_t sourceWidth = 0;
    int32_t sourceHeight = 0;
    QCamera3OppoPlatformFaceRect faces[kOppoPlatformMaxFaces];
};

struct QCamera3OppoPlatformFrame {
    const uint8_t *data;    /* NV21 frame, modified in place */
    size_t length;
    int32_t width;
    int32_t height;
    int32_t stride;
    int32_t slice;
    QCamera3OppoPlatformFaceInfo faceInfo;
    int32_t beautyStrength = 0; /* 0..100, only used by beauty modes */
};

class QCamera3OppoPlatformHwi {
public:
    /* True when either probe gate is on; get() is only valid then. */
    static bool configured();
    /* True when the explicit-mode run gate is on (persist...platform_hwi.run==1). */
    static bool runEnabled();
    static QCamera3OppoPlatformHwi &get();
    /* Union of the original algorithm bits the HAL3 integration can request. */
    static uint64_t sessionAlgorithmMask();

    void onOpenCamera(int32_t cameraId, bool isFront);
    static void shutdownOnClose();

    /* Probe path: runs the configured mask on one snapshot frame in place. */
    bool processFrame(const QCamera3OppoPlatformFrame &frame);

    /*
     * Runs the original single-frame algorithm selected by an explicit
     * org.lineageos stillMode (HDR/LLHDR/MFNR/SUPER_PHOTO/REAR_BEAUTY/
     * DUAL_BOKEH/SCENE/FRONT_BEAUTY). Returns 0 when the platform block ran,
     * -EOPNOTSUPP when the gate/back end is unavailable (caller falls back),
     * another negative errno on failure. The frame is modified in place.
     */
    int32_t processStillFrame(int32_t stillMode, const QCamera3OppoPlatformFrame &frame);

    /* Last processStillFrame() result, for diagnostics. */
    int32_t lastStatus() const;

    /* True when the beauty model path for this mode was found on the device
     * (RE task-74: never run with an empty/nonexistent path). processStillFrame
     * for 5/10 returns -EOPNOTSUPP while false; callers fall back. */
    bool beautyParamsReady(int32_t stillMode) const;

    /* Face result cache fed from HAL3 CAM_INTF_META_FACE_DETECTION (active-array
     * rects). getFaceInfo() prefers the exact frame; otherwise it uses the latest
     * result within a bounded frame age (then count may be 0 = honestly no face). */
    static void setFaceInfo(uint32_t frameNumber,
            const QCamera3OppoPlatformFaceInfo &info);
    static bool getFaceInfo(uint32_t frameNumber, QCamera3OppoPlatformFaceInfo &info);

private:
    QCamera3OppoPlatformHwi();
    ~QCamera3OppoPlatformHwi();
    QCamera3OppoPlatformHwi(const QCamera3OppoPlatformHwi &) = delete;
    QCamera3OppoPlatformHwi &operator=(const QCamera3OppoPlatformHwi &) = delete;

    void onCloseCamera();
    /* NULL = stage 1 probe with zeroed geometry; else stage 2 with the real geometry. */
    bool initializeLocked(const QCamera3OppoPlatformFrame *frame);
    void shutdownLocked();
    void buildPlatformParamLocked(const QCamera3OppoPlatformFrame *frame);
    void buildFrameCtxLocked(const QCamera3OppoPlatformFrame &frame);
    void dumpFrameLocked(const QCamera3OppoPlatformFrame &frame, const char *tag);

    void *mLib;
    void *mInit;
    void *mProcess;
    void *mUninit;
    void *mUpdateParameters;
    void *mSetCallbacks = nullptr;

    pthread_mutex_t mLock;
    uint8_t *mParam;        /* param_platform_t block, 0x12EC bytes */
    uint8_t *mFrameCtx;     /* frame ctx block, 0x308 bytes */
    size_t mParamSize;
    size_t mFrameCtxSize;

    uint64_t mMask;         /* configured algorithm mask, without bit63 */
    int32_t mCameraId;
    bool mIsFront;
    bool mInitialized;
    bool mUnavailable;
    uint32_t mFrameCount;
    int32_t mLastStatus = -EOPNOTSUPP;
    /* Original beauty parameters (task-74). Paths are resolved on camera open
     * from properties or existing device files; empty = beauty not registered. */
    char mBeautyModelRear[0x80] = {};
    char mBeautyModelFront[0x80] = {};
    int32_t mBeautyParam24 = 0;
    int32_t mBeautyThreshold1 = 0;
    int32_t mBeautyThreshold2 = 0;
    int32_t mBeautyMode = 0;
    int32_t mBeautyEnable = 0;
    int32_t mFaceFdSelect = 0;
    int32_t mFaceInfoMode = 0;
    int32_t mScesParam = 0;
    void resolveBeautyParamsLocked();
    /* Session-level scratch buffer for frame ctx +0x2c8/2cc/2d4 (0x1000 B,
     * 4 KiB aligned). facebeauty/imageadjust/superphoto/blurless read it. */
    void *mScratch = nullptr;
    int32_t mScratchFd = -1;
    size_t mScratchSize = 0x1000;
    bool ensureScratchLocked();
    void freeScratchLocked();
};

#endif /* QCAMERA3_OPPO_PLATFORM_HWI_H */
