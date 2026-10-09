/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_OPPO_PLATFORM_HWI_H
#define QCAMERA3_OPPO_PLATFORM_HWI_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

/*
 * In-process wiring of the OPPO platform algorithm layer (liboppo_platform_hwi.so).
 * Off by default: the persist.vendor.camera.oppo.platform_hwi[.process] gates enable it.
 */

struct QCamera3OppoPlatformFrame {
    const uint8_t *data;    /* NV21 frame, modified in place */
    size_t length;
    int32_t width;
    int32_t height;
    int32_t stride;
    int32_t slice;
};

class QCamera3OppoPlatformHwi {
public:
    /* True when either gate is on; get() is only valid then. */
    static bool configured();
    static QCamera3OppoPlatformHwi &get();

    void onOpenCamera(int32_t cameraId, bool isFront);
    static void shutdownOnClose();

    /* Runs the configured mask on one snapshot frame in place; failure is a no-op. */
    bool processFrame(const QCamera3OppoPlatformFrame &frame);

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
};

#endif /* QCAMERA3_OPPO_PLATFORM_HWI_H */
