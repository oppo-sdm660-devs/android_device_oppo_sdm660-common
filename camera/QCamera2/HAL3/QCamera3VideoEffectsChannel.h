/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_VIDEO_EFFECTS_CHANNEL_H
#define QCAMERA3_VIDEO_EFFECTS_CHANNEL_H
#include "QCamera3Channel.h"
#include "QCamera3VideoEffects.h"
namespace qcamera {
// CPU effects consume a private linear NV12 acquisition buffer. Framework
// buffers are output-only and remain leased until the worker returns them.
class QCamera3VideoEffectsChannel : public QCamera3RegularChannel {
public:
    QCamera3VideoEffectsChannel(uint32_t cameraHandle, uint32_t channelHandle,
        mm_camera_ops_t *ops, channel_cb_routine callback,
        channel_cb_buffer_err bufferError, cam_padding_info_t *padding, void *user,
        camera3_stream_t *stream, cam_stream_type_t type, cam_feature_mask_t features,
        QCamera3Channel *metadataChannel);
    ~QCamera3VideoEffectsChannel() override;
    bool usesVideoEffects() const override { return true; }
    // HWI freezes this once for every eligible channel, before any initialize.
    void setVideoEffectsEnabled(bool enabled) override { mCpuEnabled = enabled; }
    void setVideoEffects(const QCamera3VideoEffects::Options &options) override;
    int32_t initialize(cam_is_type_t isType) override;
    int32_t start() override;
    int32_t stop() override;
    // HWI flushPerf calls this only after every buffer completion callback;
    // mapper unlock precedes that callback. Full HWI flush calls stop()/join()
    // before returning pending buffers. No live output lease is released here.
    int32_t flush() override { return mCpuEnabled ? 0 : QCamera3RegularChannel::flush(); }
    int32_t request(buffer_handle_t *buffer, uint32_t frameNumber, int &indexUsed) override;
    int32_t timeoutFrame(uint32_t frameNumber) override;
    void streamCbRoutine(mm_camera_super_buf_t *frame, QCamera3Stream *stream) override;
    QCamera3StreamMem *getStreamBufs(uint32_t length) override;
    void putStreamBufs() override;
private:
    static constexpr unsigned kBuffers = 8;
    struct Pending {
        buffer_handle_t *buffer;
        int outputIndex;
        uint32_t frameNumber;
        uint64_t order;
        int64_t queuedNs;
        bool active, imageReady, failed;
        QCamera3VideoEffects::Image image;
        QCamera3VideoEffects::Options effects;
    } mPending[kBuffers] = {};
    bool mInputBusy[kBuffers] = {};
    QCamera3StreamMem mInputMemory;
    QCamera3VideoEffects::Options mEffects;
    std::atomic<bool> mCancelled{true};
    pthread_mutex_t mLock = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t mCondition = PTHREAD_COND_INITIALIZER;
    pthread_t mWorker = {};
    bool mWorkerStarted = false, mStopping = true;
    bool mCpuEnabled = false;
    uint64_t mOrder = 0;
    static void *workerEntry(void *user);
    void run();
    int32_t output(Pending &pending);
};
}
#endif
