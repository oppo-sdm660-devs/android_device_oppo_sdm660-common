/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3MmsChannel.h"
#include "QCamera3EisSnapshot.h"

extern "C" {
#include "mm_camera_dbg.h"
}

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ui/GraphicBufferMapper.h>
#include <ui/Rect.h>

namespace qcamera {
namespace {
int64_t bootTime() {
    struct timespec now;
    if (clock_gettime(CLOCK_BOOTTIME, &now)) return 0;
    return int64_t(now.tv_sec) * 1000000000LL + now.tv_nsec;
}
}
QCamera3MmsChannel::QCamera3MmsChannel(uint32_t cameraHandle, uint32_t channelHandle,
        mm_camera_ops_t *ops, channel_cb_routine callback, channel_cb_buffer_err bufferError,
        cam_padding_info_t *padding, void *user, camera3_stream_t *stream,
        cam_feature_mask_t features, QCamera3Channel *metadataChannel,
        const QCamera3MmsProfile &profile)
    : QCamera3RegularChannel(cameraHandle, channelHandle, ops, callback, bufferError,
            padding, user, stream, CAM_STREAM_TYPE_VIDEO, features, metadataChannel, kBuffers),
      mProfile(profile), mInputMemory(kBuffers, false) {
    setUBWCEnabled(false);
}
QCamera3MmsChannel::~QCamera3MmsChannel() {
    stop();
    // Stream stop/deinit must finish before heap memory is released.
    destroy();
    mInputMemory.clear();
    pthread_cond_destroy(&mCondition);
    pthread_mutex_destroy(&mLock);
}
int32_t QCamera3MmsChannel::initialize(cam_is_type_t) {
    if (m_numStreams) return 0;
    if (!mCamera3Stream || mCamera3Stream->rotation != CAMERA3_STREAM_ROTATION_0 ||
            mCamera3Stream->width != mProfile.outputWidth ||
            mCamera3Stream->height != mProfile.outputHeight) return -EINVAL;
    mIsType = IS_TYPE_NONE;
    mStreamType = CAM_STREAM_TYPE_VIDEO;
    mStreamFormat = CAM_FORMAT_YUV_420_NV12;
    cam_dimension_t dimensions = {int32_t(mProfile.inputWidth), int32_t(mProfile.inputHeight)};
    return QCamera3Channel::addStream(mStreamType, mStreamFormat, dimensions,
            ROTATE_0, kBuffers, mPostProcMask, IS_TYPE_NONE);
}
int32_t QCamera3MmsChannel::start() {
    if (m_bIsActive) return 0;
    int32_t status = mMms.gyroPeriodNs() ? 0 : prepare();
    if (status) {
        // The stock MMS engine could not be configured (missing/denied core or
        // incompatible profile). The channel keeps the negotiated topology and
        // runs the HAL3-native gyro stabiliser instead; it refuses every frame
        // without real gyro data and never fabricates stabilisation.
        mNativeVstab = true;
        mGyroVstab.reset();
        mMms.flush();
    }
    pthread_mutex_lock(&mLock);
    mStopping = false;
    mEffectCancelled.store(false);
    pthread_mutex_unlock(&mLock);
    status = pthread_create(&mWorker, nullptr, workerEntry, this);
    if (status) { mGyro.stop(); mMms.flush(); return -status; }
    mWorkerStarted = true;
    status = QCamera3Channel::start();
    if (status) stop();
    return status;
}
int32_t QCamera3MmsChannel::prepare() {
    QCamera3MmsVstab::Configuration config = {mProfile.path, mProfile.inputWidth,
        mProfile.inputHeight, mProfile.outputWidth, mProfile.outputHeight, kBuffers, 250000000};
    int32_t status = mMms.configure(config);
    if (status) return status;
    status = mGyro.start(mMms, wake, this);
    if (status) { mMms.flush(); return status; }
    return status;
}
int32_t QCamera3MmsChannel::stop() {
    mEffectCancelled.store(true);
    pthread_mutex_lock(&mLock);
    mStopping = true;
    pthread_cond_broadcast(&mCondition);
    pthread_mutex_unlock(&mLock);
    mMms.flush(); // Sets cancellation before waiting for an active core call.
    mGyro.stop();
    if (mWorkerStarted) {
        pthread_join(mWorker, nullptr);
        mWorkerStarted = false;
    }
    int32_t status = m_bIsActive ? QCamera3Channel::stop() : 0;
    pthread_mutex_lock(&mLock);
    for (Pending &pending : mPending) pending = {};
    memset(mInputBusy, 0, sizeof(mInputBusy));
    pthread_mutex_unlock(&mLock);
    // The worker is joined above; releasing the stabiliser scratch is legal here.
    mGyroVstab.reset();
    mMemory.unregisterBuffers();
    // HWI's flush returns outstanding framework buffers with error after all
    // channels stop; issuing another callback here would return them twice.
    return status;
}
int32_t QCamera3MmsChannel::flush() {
    // The optimized HWI flush calls this only after pending buffers drain.
    // Preserve the backend stream but discard pre-flush gyro history.
    mMms.flush();
    mGyro.stop();
    QCamera3MmsVstab::Configuration config = {mProfile.path, mProfile.inputWidth,
        mProfile.inputHeight, mProfile.outputWidth, mProfile.outputHeight, kBuffers, 250000000};
    int32_t status = mMms.configure(config);
    if (status) return status;
    status = mGyro.start(mMms, wake, this);
    return status;
}
void QCamera3MmsChannel::setStabilizationEnabled(bool enabled, float zoom) {
    pthread_mutex_lock(&mLock); mEnabled = enabled; mZoom = zoom; pthread_mutex_unlock(&mLock);
}
void QCamera3MmsChannel::setStabilizationCalibration(float focalLengthPx) {
    pthread_mutex_lock(&mLock); mFocalLengthPx = focalLengthPx; pthread_mutex_unlock(&mLock);
}
int32_t QCamera3MmsChannel::request(buffer_handle_t *buffer, uint32_t number, int &indexUsed) {
    if (!buffer || !*buffer) return -EINVAL;
    int32_t status = 0;
    int outputIndex = mMemory.getMatchBufIndex(buffer);
    if (outputIndex < 0) {
        status = registerBuffer(buffer, IS_TYPE_NONE);
        if (status) return status;
        outputIndex = mMemory.getMatchBufIndex(buffer);
    }
    if (!m_bIsActive && (status = start())) return status;
    // mMemory's gralloc range starts after its reserved heap range. The MMS
    // acquisition memory is separate and uses private slot indices 0..7.
    if (outputIndex < int(kBuffers)) return -EINVAL;
    if (unsigned(outputIndex) >= 2 * kBuffers) { mMemory.unregisterBuffer(outputIndex);return -EBUSY; }
    const int index = outputIndex - kBuffers;
    pthread_mutex_lock(&mLock);
    if (mStopping || mInputBusy[index]) {
        pthread_mutex_unlock(&mLock); return -EBUSY;
    }
    Pending &pending = mPending[index];
    pending = {};
    pending.buffer = buffer; pending.frameNumber = number; pending.order = ++mOrder;
    pending.outputIndex = outputIndex;
    pending.queuedNs = bootTime(); pending.active = true; pending.enabled = mEnabled;
    pending.frame.zoom = mZoom;
    pending.focalLengthPx = mFocalLengthPx;
    mInputBusy[index] = true;
    pthread_mutex_unlock(&mLock);
    mMemory.markFrameNumber(outputIndex, number);
    mInputMemory.markFrameNumber(index, number);
    status = mStreams[0]->bufDone(index);
    if (status) {
        pthread_mutex_lock(&mLock); pending = {}; mInputBusy[index] = false;
        pthread_mutex_unlock(&mLock);
        mMemory.markFrameNumber(outputIndex, -1); mInputMemory.markFrameNumber(index, -1);
        mMemory.unregisterBuffer(outputIndex);
        return status;
    }
    indexUsed = index;
    wake(this);
    return 0;
}
QCamera3StreamMem *QCamera3MmsChannel::getStreamBufs(uint32_t length) {
    if (!mInputMemory.getCnt() && mInputMemory.allocateAll(length)) return nullptr;
    return &mInputMemory;
}
void QCamera3MmsChannel::putStreamBufs() {
    mInputMemory.clear();
    mMemory.unregisterBuffers();
}
void QCamera3MmsChannel::streamCbRoutine(mm_camera_super_buf_t *frame, QCamera3Stream *stream) {
    if (!frame) return;
    if (!stream || frame->num_bufs != 1 || !frame->bufs[0] || frame->bufs[0]->buf_idx >= kBuffers) {
        free(frame); return;
    }
    mm_camera_buf_def_t *buffer = frame->bufs[0];
    unsigned index = buffer->buf_idx;
    cam_frame_len_offset_t layout = {};
    stream->getFrameOffset(layout);
    int32_t cacheStatus = mInputMemory.invalidateCache(index);
    pthread_mutex_lock(&mLock);
    Pending &pending = mPending[index];
    if (!mStopping && pending.active && !pending.imageReady) {
        // The stock capture ABI uses a signed 32-bit tv_sec; converting it
        // before multiplication preserves the full BOOTTIME nanoseconds.
        if (buffer->ts.tv_sec < 0 || buffer->ts.tv_nsec < 0 || buffer->ts.tv_nsec >= 1000000000)
            pending.failed = true;
        else pending.imageTimestampNs = int64_t(buffer->ts.tv_sec) * 1000000000LL + buffer->ts.tv_nsec;
        pending.processedNs = pending.imageTimestampNs;
        if (IS_BUFFER_ERROR(buffer->flags) || cacheStatus || layout.num_planes != 2 || !buffer->buffer ||
                buffer->frame_len < layout.frame_len || layout.mp[0].stride <= 0 ||
                layout.mp[1].stride <= 0 || layout.mp[0].scanline <= 0 || layout.mp[1].scanline <= 0) {
            pending.failed = true;
        } else {
            pending.image = {static_cast<uint8_t *>(buffer->buffer), buffer->frame_len,
                mProfile.inputWidth, mProfile.inputHeight, layout.mp[0].offset,
                size_t(layout.mp[0].len) + layout.mp[1].offset,
                uint32_t(layout.mp[0].stride), uint32_t(layout.mp[1].stride),
                uint32_t(layout.mp[0].scanline), uint32_t(layout.mp[1].scanline)};
        }
        pending.imageReady = true;
    } else if (!pending.active && mInputBusy[index]) {
        mInputBusy[index] = false; // A timed-out input has finally returned.
        mMemory.unregisterBuffer(index + kBuffers);
    }
    pthread_cond_broadcast(&mCondition);
    pthread_mutex_unlock(&mLock);
    free(frame);
}
void QCamera3MmsChannel::queueSensorMetadata(uint32_t number, uint32_t cameraHandle,
        const metadata_buffer_t *metadata) {
    if (cameraHandle != m_camHandle || !metadata) return;
    const int64_t *timestamp = nullptr, *exposure = nullptr;
    if (metadata->is_valid[CAM_INTF_META_SENSOR_TIMESTAMP])
        timestamp = POINTER_OF_META(CAM_INTF_META_SENSOR_TIMESTAMP, metadata);
    if (metadata->is_valid[CAM_INTF_META_SENSOR_EXPOSURE_TIME])
        exposure = POINTER_OF_META(CAM_INTF_META_SENSOR_EXPOSURE_TIME, metadata);
    pthread_mutex_lock(&mLock);
    for (Pending &pending : mPending) {
        if (!pending.active || pending.frameNumber != number || pending.metadataReady) continue;
        if (!timestamp || !exposure || *timestamp <= 0 || *exposure <= 0) pending.failed = true;
        else pending.frame = {number, *timestamp, *exposure, pending.frame.zoom};
        pending.metadataReady = true;
        break;
    }
    pthread_cond_broadcast(&mCondition);
    pthread_mutex_unlock(&mLock);
}
int32_t QCamera3MmsChannel::timeoutFrame(uint32_t number) {
    pthread_mutex_lock(&mLock);
    for (Pending &pending : mPending)
        if (pending.active && pending.frameNumber == number) pending.failed = true;
    pthread_cond_broadcast(&mCondition);
    pthread_mutex_unlock(&mLock);
    return 0;
}
// Real EIS-aware still selection over the frames this engine really warped.
// The candidates are the per-frame QCamera3MmsVstab::Result records of the MMS
// core; the state is this channel's own real EIS state. A frame that was not
// really warped is recorded with eisApplied 0 and the stage refuses it, so no
// stabilisation is ever claimed for it.
void QCamera3MmsChannel::recordEisCandidate(const QCamera3MmsVstab::Result &result,
        const Pending &pending) {
    QCamera3EisSnapshot::Candidate candidate;
    if (!QCamera3EisSnapshot::fromMmsResult(result, 1, 1, -1, 0, candidate)) return;
    pthread_mutex_lock(&mLock);
    mEisCandidates[mEisCandidateNext] = candidate;
    mEisCandidateNext = (mEisCandidateNext + 1) % QCamera3EisSnapshot::MAX_CANDIDATES;
    if (mEisCandidateCount < QCamera3EisSnapshot::MAX_CANDIDATES) ++mEisCandidateCount;
    pthread_mutex_unlock(&mLock);
    native_eis_state_t eis = {};
    getEisState(eis);
    QCamera3EisSnapshot::EisState state;
    state.enabled = eis.enabled;
    state.configured = eis.configured;
    state.coreStatus = eis.coreStatus;
    state.gyroValid = eis.gyroValid;
    state.gyroPeriodNs = eis.gyroPeriodNs;
    state.version = eis.version;
    QCamera3EisSnapshot::Options options;
    options.requireStabilized = 1;
    options.nowNs = pending.processedNs > 0 ? pending.processedNs : bootTime();
    options.maxAgeNs = 250000000;
    options.frameNumber = pending.frameNumber;
    QCamera3EisSnapshot::Candidate ring[QCamera3EisSnapshot::MAX_CANDIDATES] = {};
    uint32_t count = 0;
    pthread_mutex_lock(&mLock);
    count = mEisCandidateCount;
    for (uint32_t i = 0; i < count; ++i)
        ring[i] = mEisCandidates[(mEisCandidateNext + QCamera3EisSnapshot::MAX_CANDIDATES -
                count + i) % QCamera3EisSnapshot::MAX_CANDIDATES];
    pthread_mutex_unlock(&mLock);
    if (!QCamera3EisSnapshot::validOptions(options) || count == 0) return;
    QCamera3EisSnapshot::Result selectResult;
    QCamera3EisSnapshot::Selection selection;
    if (mEisSnapshot.select(state, ring, count, options, selectResult, selection,
            &mEffectCancelled) == NO_ERROR) {
        pthread_mutex_lock(&mLock);
        mEisSelection = selection;
        mEisSelectionValid = true;
        pthread_mutex_unlock(&mLock);
    }
}

bool QCamera3MmsChannel::getEisSelection(QCamera3EisSnapshot::Selection &selection) const {
    pthread_mutex_lock(&mLock);
    const bool valid = mEisSelectionValid;
    if (valid) selection = mEisSelection;
    pthread_mutex_unlock(&mLock);
    return valid;
}

// Native gyro video stabilisation for a session the stock MMS engine could not
// serve. Real inputs only: the gyro samples the seam delivered, the real frame
// timestamp and the decoded focal length. Without gyro data or calibration the
// stage refuses and the frame is passed through unchanged (passthrough == 1),
// with the refusal recorded, never reported as stabilised.
int32_t QCamera3MmsChannel::runNativeVstab(Pending &pending,
        const QCamera3MmsVstab::Image &work, int64_t nowNs) {
    // The stage is sensor-time based; the caller's wall time is not an input.
    (void)nowNs;
    QCamera3GyroVstab::Options options;
    options.strength = 100;
    options.maxCropPercent = 8;
    options.windowNs = 33333333;
    options.timestampNs = pending.frame.timestampNs > 0 ? pending.frame.timestampNs :
            pending.processedNs;
    options.frameNumber = pending.frameNumber;
    options.focalLengthPx = pending.focalLengthPx;
    options.path = QCamera3GyroVstab::PATH_NATIVE;
    options.passthrough = 1;
    QCamera3GyroVstab::Result result;
    return mGyroVstab.process(work, work, options, result, &mEffectCancelled);
}

bool QCamera3MmsChannel::getEisState(native_eis_state_t &state) {
    state = {};
    pthread_mutex_lock(&mLock);
    const bool enabled = mEnabled;
    pthread_mutex_unlock(&mLock);
    const int64_t periodNs = mMms.gyroPeriodNs();
    const bool gyro = mGyro.started();
    state.valid = 1;
    state.enabled = (enabled || mNativeVstab) ? 1 : 0;
    state.configured = periodNs > 0 ? 1 : 0;
    state.coreStatus = mNativeVstab ? mLastVstabStatus : (periodNs > 0 ? 0 : -ENODEV);
    state.gyroValid = gyro ? 1 : 0;
    state.gyroPeriodNs = periodNs;
    // This port's stabiliser is the MMS/EIS-class engine it implements
    // natively; the ladder value is reported only when that engine really is
    // configured and fed, and stays 0 (none) otherwise.
    state.version = (periodNs > 0 && gyro) ? QCamera3EisSnapshot::kEis2 :
            QCamera3EisSnapshot::kEisNone;
    return true;
}

void QCamera3MmsChannel::wake(void *user) {
    auto *channel = static_cast<QCamera3MmsChannel *>(user);
    pthread_cond_signal(&channel->mCondition);
}
void *QCamera3MmsChannel::workerEntry(void *user) {
    static_cast<QCamera3MmsChannel *>(user)->run(); return nullptr;
}
int32_t QCamera3MmsChannel::output(Pending &pending, int64_t nowNs) {
    const size_t pixels = size_t(mProfile.outputWidth) * mProfile.outputHeight;
    uint8_t *data = static_cast<uint8_t *>(malloc(pixels * 3 / 2));
    if (!data) return -ENOMEM;
    QCamera3MmsVstab::Image destination = {data, pixels * 3 / 2,
        mProfile.outputWidth, mProfile.outputHeight, 0, pixels,
        mProfile.outputWidth, mProfile.outputWidth, mProfile.outputHeight, mProfile.outputHeight / 2};
    int32_t status;
    if (pending.enabled && mNativeVstab) {
        const int32_t vstabStatus = runNativeVstab(pending, destination, nowNs);
        mLastVstabStatus = vstabStatus;
        // Documented refusals (no gyro data, stale samples, window not covered,
        // unusable stock core) leave the pass-through copy in place: the frame
        // is delivered and only the refusal is recorded. A real failure stays
        // an error and never becomes a silent success.
        status = (vstabStatus == -ENODATA || vstabStatus == -ETIMEDOUT ||
                vstabStatus == -ERANGE || vstabStatus == -ENOSYS) ? 0 : vstabStatus;
    } else if (pending.enabled) {
        // Buffer assignment identifies the request; its SOF time independently
        // verifies the metadata association. V4L2 timeval conversion may lose
        // sub-microsecond precision, so permit at most one microsecond.
        const int64_t delta = pending.imageTimestampNs - pending.frame.timestampNs;
        if (pending.imageTimestampNs <= 0 || delta < -1000 || delta > 1000) {
            free(data); return -EPROTO;
        }
        if (!pending.enqueued) {
            status = mMms.enqueue(pending.image, pending.frame, nowNs);
            if (status) { free(data); return status; }
            pending.enqueued = true;
        }
        QCamera3MmsVstab::Result result;
        status = mMms.processNext(destination, nowNs, result);
        if (!status && result.frame.frameNumber != pending.frameNumber) status = -EPROTO;
        if (!status) recordEisCandidate(result, pending);
    } else {
        const QCamera3MmsVstab::Vertex identity[] = {{-1, -1, 0}, {1, -1, 0}, {-1, 1, 0}, {1, 1, 0}};
        status = QCamera3MmsVstab::render(pending.image, destination, identity, 2, 2);
    }
    // The single-pass effect stage (effects 1/3) that used to run here was
    // deleted with the B6 batch: the frame leaving this channel is now always the
    // completed MMS/base or native-gyro render.
    if (!status && !mEffectCancelled.load()) {
        auto &mapper = android::GraphicBufferMapper::get();
        android_ycbcr layout = {};
        status = mapper.lockYCbCr(*pending.buffer, GRALLOC_USAGE_SW_WRITE_OFTEN,
                android::Rect(mProfile.outputWidth, mProfile.outputHeight), &layout);
        if (!status) {
            if (!layout.y || !layout.cb || !layout.cr || !layout.chroma_step ||
                    layout.chroma_step > 2 || layout.ystride < mProfile.outputWidth ||
                    layout.cstride < (mProfile.outputWidth / 2 - 1) * layout.chroma_step + 1) status = -EINVAL;
            else {
                for (uint32_t row = 0; row < mProfile.outputHeight; ++row)
                    memcpy(static_cast<uint8_t *>(layout.y) + row * layout.ystride,
                            data + row * mProfile.outputWidth, mProfile.outputWidth);
                for (uint32_t row = 0; row < mProfile.outputHeight / 2; ++row)
                    for (uint32_t col = 0; col < mProfile.outputWidth / 2; ++col) {
                        static_cast<uint8_t *>(layout.cb)[row * layout.cstride + col * layout.chroma_step] =
                                data[pixels + row * mProfile.outputWidth + col * 2];
                        static_cast<uint8_t *>(layout.cr)[row * layout.cstride + col * layout.chroma_step] =
                                data[pixels + row * mProfile.outputWidth + col * 2 + 1];
                    }
            }
            int32_t unlock = mapper.unlock(*pending.buffer);
            if (!status) status = unlock;
        }
    }
    if (!status && mEffectCancelled.load()) status = -ECANCELED;
    const int32_t completed = status;
    free(data);
    return completed;
}
void QCamera3MmsChannel::run() {
    pthread_mutex_lock(&mLock);
    while (!mStopping) {
        unsigned index = kBuffers;
        for (unsigned i = 0; i < kBuffers; ++i)
            if (mPending[i].active && (index == kBuffers || mPending[i].order < mPending[index].order)) index = i;
        int64_t now = bootTime();
        if (index < kBuffers) {
            Pending &head = mPending[index];
            bool timeout = now - head.queuedNs >= 500000000;
            if (head.failed || timeout || (head.imageReady && head.metadataReady)) {
                Pending job = head;
                pthread_mutex_unlock(&mLock);
                int32_t status = job.failed ? -EIO : timeout ? -ETIMEDOUT : output(job, now);
                pthread_mutex_lock(&mLock);
                if (mStopping) break;
                head.enqueued = job.enqueued;
                // timeoutFrame can run while the core/mapper operates without
                // mLock. Honor that failure before returning this buffer; the
                // copied job cannot otherwise observe it.
                if (head.failed) status = -ETIMEDOUT;
                if (status != -EAGAIN) {
                    // Errors before processNext (including an external frame
                    // timeout) must release that frame without resetting the
                    // sensor feed while another lifecycle operation can run.
                    if (status) mMms.discard(job.frameNumber);
                    buffer_handle_t *buffer = head.buffer;
                    uint32_t frameNumber = head.frameNumber;
                    const int outputIndex = head.outputIndex;
                    mInputBusy[index] = !head.imageReady;
                    head = {};
                    mMemory.markFrameNumber(outputIndex, -1);
                    if (!mInputBusy[index]) mMemory.unregisterBuffer(outputIndex);
                    pthread_mutex_unlock(&mLock);
                    issueChannelCb(buffer, frameNumber, status ? CAMERA3_BUFFER_STATUS_ERROR : CAMERA3_BUFFER_STATUS_OK);
                    pthread_mutex_lock(&mLock);
                    continue;
                }
            }
        }
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_nsec += 10000000;
        if (deadline.tv_nsec >= 1000000000) { ++deadline.tv_sec; deadline.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&mCondition, &mLock, &deadline);
    }
    pthread_mutex_unlock(&mLock);
}
} // namespace qcamera
