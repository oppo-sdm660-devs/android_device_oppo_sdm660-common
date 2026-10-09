/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_MMS_CHANNEL_H
#define QCAMERA3_MMS_CHANNEL_H

#include "QCamera3Channel.h"
#include "QCamera3MmsProfile.h"
#include "QCamera3MmsVstab.h"
#include "QCamera3MmsGyroSource.h"
#include "QCamera3EisSnapshot.h"
#include "QCamera3GyroVstab.h"

namespace qcamera {
// Video-only channel; HWI sends this stream IS_TYPE_NONE so MMS and backend EIS
// do not both crop it.
class QCamera3MmsChannel : public QCamera3RegularChannel {
public:
    QCamera3MmsChannel(uint32_t cameraHandle, uint32_t channelHandle,
            mm_camera_ops_t *ops, channel_cb_routine callback,
            channel_cb_buffer_err bufferError, cam_padding_info_t *padding,
            void *user, camera3_stream_t *stream, cam_feature_mask_t features,
            QCamera3Channel *metadataChannel, const QCamera3MmsProfile &profile);
    ~QCamera3MmsChannel() override;
    int32_t prepare();
    // Per-frame stabiliser diagnostics for the vstab* result keys.
    // path 1 = HAL3-native gyro warper, path 2 = stock MMS core.
    struct VstabDiagnostics {
        int32_t path = 0;
        int32_t reason = 0;
        int32_t gyroCount = -1;
        int32_t coreStatus = 0;
        // 0 = CPU reference renderer, 1 = GPU; gpuStatus is the fallback errno.
        int32_t renderer = 0;
        int32_t gpuStatus = 0;
    };
    bool getVstabDiagnostics(VstabDiagnostics &diagnostics) const;
    void recordVstabDiagnostics(int32_t path, int32_t reason, int32_t gyroCount,
            int32_t coreStatus, int32_t renderer, int32_t gpuStatus);
    bool usesMmsVstab() const override { return true; }
    // Focal length in destination pixels; 0 = not decoded (frames are refused).
    void setStabilizationCalibration(float focalLengthPx);
    bool getEisState(native_eis_state_t &state) override;
    // Newest EIS still selection over the frames this engine warped.
    bool getEisSelection(QCamera3EisSnapshot::Selection &selection) const;
    int32_t initialize(cam_is_type_t isType) override;
    int32_t start() override;
    int32_t stop() override;
    int32_t flush() override;
    int32_t request(buffer_handle_t *buffer, uint32_t frameNumber, int &indexUsed) override;
    void streamCbRoutine(mm_camera_super_buf_t *frame, QCamera3Stream *stream) override;
    QCamera3StreamMem *getStreamBufs(uint32_t length) override;
    void putStreamBufs() override;
    void queueSensorMetadata(uint32_t frameNumber, uint32_t cameraHandle,
            const metadata_buffer_t *metadata) override;
    int32_t timeoutFrame(uint32_t frameNumber) override;
    // Called by HWI before request() with the standard Camera2 request state.
    void setStabilizationEnabled(bool enabled, float zoom);
    const QCamera3MmsProfile &profile() const { return mProfile; }
private:
    static constexpr unsigned kBuffers = 8;
    struct Pending {
        buffer_handle_t *buffer;
        int outputIndex;
        uint32_t frameNumber;
        uint64_t order;
        int64_t queuedNs;
        int64_t imageTimestampNs;
        int64_t processedNs;
        bool active, imageReady, metadataReady, enqueued, enabled, failed;
        QCamera3MmsVstab::Image image;
        QCamera3MmsVstab::Frame frame;
        float focalLengthPx = 0.0f;
    } mPending[kBuffers] = {};
    bool mInputBusy[kBuffers] = {};
    QCamera3MmsProfile mProfile;
    QCamera3StreamMem mInputMemory;
    QCamera3MmsVstab mMms;
    QCamera3MmsGyroSource mGyro;
    // Native gyro stabiliser used when the stock MMS engine is unusable.
    QCamera3GyroVstab mGyroVstab;
    bool mNativeVstab = false;
    int32_t mLastVstabStatus = 0;
    QCamera3EisSnapshot::Candidate mEisCandidates[QCamera3EisSnapshot::MAX_CANDIDATES] = {};
    uint32_t mEisCandidateCount = 0;
    uint32_t mEisCandidateNext = 0;
    QCamera3EisSnapshot mEisSnapshot;
    QCamera3EisSnapshot::Selection mEisSelection;
    bool mEisSelectionValid = false;
    VstabDiagnostics mVstabDiagnostics = {};
    bool mVstabDiagnosticsValid = false;
    mutable pthread_mutex_t mLock = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t mCondition = PTHREAD_COND_INITIALIZER;
    pthread_t mWorker = {};
    bool mWorkerStarted = false, mStopping = true, mEnabled = false;
    float mZoom = 1.0f;
    float mFocalLengthPx = 0.0f;
    std::atomic<bool> mEffectCancelled{true};
    uint64_t mOrder = 0;
    static void *workerEntry(void *user);
    static void wake(void *user);
    void run();
    int32_t output(Pending &pending, int64_t nowNs);
    // Records one warped frame as an EIS candidate and runs the selection.
    void recordEisCandidate(const QCamera3MmsVstab::Result &result, const Pending &pending);
    int32_t runNativeVstab(Pending &pending, const QCamera3MmsVstab::Image &image,
            int64_t nowNs);
    // Records the execution unit of a pass-through frame.
    void recordVstabUnit(int32_t renderer, int32_t gpuStatus);
    // Delivers the frame un-stabilised instead of an ERROR buffer.
    int32_t outputPassthrough(Pending &pending, int32_t reason);
    // Locks the destination gralloc buffer and writes the rendered NV12 frame back.
    int32_t writeBack(Pending &pending, const uint8_t *data);
    // Rate-limited log (persist.vendor.camera.oppo.vstab.debug, default 1).
    void logVstabFrame(const Pending &pending, int32_t path, int32_t coreStatus,
            int32_t fallbackReason, bool degraded, int32_t renderer, int32_t gpuStatus,
            int64_t gyroDeltaNs, bool gyroDeltaValid, int32_t addGyroFailures);
    std::atomic<bool> mDebugLog{true};
    uint64_t mLogCounter = 0;
};
} // namespace qcamera
#endif
