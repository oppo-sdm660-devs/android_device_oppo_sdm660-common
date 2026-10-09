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
// A video-only channel with separately negotiated acquisition and framework
// geometry. HWI must send profile.inputWidth/Height in CAM_INTF_META_STREAM_INFO
// and IS_TYPE_NONE for this stream: MMS and backend EIS must not both crop it.
class QCamera3MmsChannel : public QCamera3RegularChannel {
public:
    QCamera3MmsChannel(uint32_t cameraHandle, uint32_t channelHandle,
            mm_camera_ops_t *ops, channel_cb_routine callback,
            channel_cb_buffer_err bufferError, cam_padding_info_t *padding,
            void *user, camera3_stream_t *stream, cam_feature_mask_t features,
            QCamera3Channel *metadataChannel, const QCamera3MmsProfile &profile);
    ~QCamera3MmsChannel() override;
    int32_t prepare();
    bool usesMmsVstab() const override { return true; }
    // Real calibration of the streaming output for the gyro stabiliser, in
    // destination pixels: f_px = width / (2 * tan(horizontal_fov / 2)), decoded
    // from the capability. 0 means "not decoded": the stabiliser then refuses the
    // frames instead of guessing. It used to travel inside the effect options.
    void setStabilizationCalibration(float focalLengthPx);
    // Real EIS/MMS state of this seam.
    bool getEisState(native_eis_state_t &state) override;
    // Real EIS still selection over the frames this engine really warped. The
    // selection is the newest real decision; false when none was produced yet.
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
    // Called by HWI before request(), using the effective standard Camera2
    // request including repeating-request inheritance. No OPPO private tag.
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
    // Native gyro stabiliser used when the stock MMS engine is not usable for
    // this session. It refuses every frame without real gyro data and never
    // claims stabilisation it did not apply.
    QCamera3GyroVstab mGyroVstab;
    bool mNativeVstab = false;
    // Last real status of the native stabiliser (QCamera3GyroVstab Result).
    int32_t mLastVstabStatus = 0;
    // Bounded real candidate ring of the MMS results this engine produced, and
    // the newest real EIS still selection over them.
    QCamera3EisSnapshot::Candidate mEisCandidates[QCamera3EisSnapshot::MAX_CANDIDATES] = {};
    uint32_t mEisCandidateCount = 0;
    uint32_t mEisCandidateNext = 0;
    QCamera3EisSnapshot mEisSnapshot;
    QCamera3EisSnapshot::Selection mEisSelection;
    bool mEisSelectionValid = false;
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
    // Records one real MMS result as an EIS still-selection candidate and runs
    // the real selection over the bounded ring.
    void recordEisCandidate(const QCamera3MmsVstab::Result &result, const Pending &pending);
    int32_t runNativeVstab(Pending &pending, const QCamera3MmsVstab::Image &image,
            int64_t nowNs);
};
} // namespace qcamera
#endif
