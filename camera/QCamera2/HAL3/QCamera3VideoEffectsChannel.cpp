/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3VideoEffectsChannel.h"
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
QCamera3VideoEffectsChannel::QCamera3VideoEffectsChannel(uint32_t cameraHandle,
        uint32_t channelHandle, mm_camera_ops_t *ops, channel_cb_routine callback,
        channel_cb_buffer_err bufferError, cam_padding_info_t *padding, void *user,
        camera3_stream_t *stream, cam_stream_type_t type, cam_feature_mask_t features,
        QCamera3Channel *metadataChannel)
    : QCamera3RegularChannel(cameraHandle, channelHandle, ops, callback, bufferError,
        padding, user, stream, type, features, metadataChannel, kBuffers),
      mInputMemory(kBuffers, false) {}
QCamera3VideoEffectsChannel::~QCamera3VideoEffectsChannel() {
    stop();destroy();mInputMemory.clear();
    pthread_cond_destroy(&mCondition);pthread_mutex_destroy(&mLock);
}
int32_t QCamera3VideoEffectsChannel::initialize(cam_is_type_t isType) {
    if (!mCpuEnabled) return QCamera3RegularChannel::initialize(isType);
    if (m_numStreams) return 0;
    if (!mCamera3Stream || mCamera3Stream->rotation != CAMERA3_STREAM_ROTATION_0 ||
        !QCamera3VideoEffects::supports(mCamera3Stream->width, mCamera3Stream->height)) return -EINVAL;
    mIsType = isType;mStreamFormat = CAM_FORMAT_YUV_420_NV12;
    cam_dimension_t dimensions = {int32_t(mCamera3Stream->width), int32_t(mCamera3Stream->height)};
    return QCamera3Channel::addStream(mStreamType, mStreamFormat, dimensions,
        ROTATE_0, kBuffers, mPostProcMask, isType);
}
void QCamera3VideoEffectsChannel::setVideoEffects(const QCamera3VideoEffects::Options &options) {
    pthread_mutex_lock(&mLock);mEffects = options;pthread_mutex_unlock(&mLock);
}
int32_t QCamera3VideoEffectsChannel::start() {
    if (!mCpuEnabled) return QCamera3RegularChannel::start();
    if (m_bIsActive) return 0;
    pthread_mutex_lock(&mLock);mStopping = false;mCancelled.store(false);pthread_mutex_unlock(&mLock);
    int32_t status = pthread_create(&mWorker, nullptr, workerEntry, this);
    if (status) { mCancelled.store(true);mStopping = true;return -status; }
    mWorkerStarted = true;status = QCamera3Channel::start();if (status) stop();return status;
}
int32_t QCamera3VideoEffectsChannel::stop() {
    if (!mCpuEnabled) return QCamera3RegularChannel::stop();
    mCancelled.store(true);
    pthread_mutex_lock(&mLock);mStopping = true;pthread_cond_broadcast(&mCondition);pthread_mutex_unlock(&mLock);
    if (mWorkerStarted) { pthread_join(mWorker, nullptr);mWorkerStarted = false; }
    int32_t status = m_bIsActive ? QCamera3Channel::stop() : 0;
    pthread_mutex_lock(&mLock);
    for (Pending &pending : mPending) pending = {};
    memset(mInputBusy, 0, sizeof(mInputBusy));pthread_mutex_unlock(&mLock);
    mMemory.unregisterBuffers();
    // HWI owns error return of outstanding framework buffers during flush.
    return status;
}
QCamera3StreamMem *QCamera3VideoEffectsChannel::getStreamBufs(uint32_t length) {
    if (!mCpuEnabled) return QCamera3RegularChannel::getStreamBufs(length);
    if (!mInputMemory.getCnt() && mInputMemory.allocateAll(length)) return nullptr;
    return &mInputMemory;
}
void QCamera3VideoEffectsChannel::putStreamBufs() {
    if (!mCpuEnabled) { QCamera3RegularChannel::putStreamBufs();return; }
    mInputMemory.clear();mMemory.unregisterBuffers();
}
int32_t QCamera3VideoEffectsChannel::request(buffer_handle_t *buffer, uint32_t number, int &indexUsed) {
    if (!mCpuEnabled) return QCamera3RegularChannel::request(buffer, number, indexUsed);
    if (!buffer || !*buffer) return -EINVAL;
    int outputIndex = mMemory.getMatchBufIndex(buffer);int32_t status = 0;
    if (outputIndex < 0) {
        status = registerBuffer(buffer, IS_TYPE_NONE);if (status) return status;
        outputIndex = mMemory.getMatchBufIndex(buffer);
    }
    if (!m_bIsActive && (status = start())) return status;
    // ProcessingChannel reserves mMemory's [0,kBuffers) for heap buffers;
    // framework registrations begin at kBuffers. Our separate input memory
    // exposes only private acquisition slots [0,kBuffers) to the backend.
    if (outputIndex < int(kBuffers)) return -EINVAL;
    if (unsigned(outputIndex) >= 2 * kBuffers) { mMemory.unregisterBuffer(outputIndex);return -EBUSY; }
    const int index = outputIndex - kBuffers;
    pthread_mutex_lock(&mLock);
    if (mStopping || mInputBusy[index]) { pthread_mutex_unlock(&mLock);return -EBUSY; }
    Pending &pending = mPending[index];pending = {};
    pending.buffer = buffer;pending.frameNumber = number;pending.order = ++mOrder;
    pending.outputIndex = outputIndex;
    pending.queuedNs = bootTime();pending.active = true;pending.effects = mEffects;
    mInputBusy[index] = true;pthread_mutex_unlock(&mLock);
    mMemory.markFrameNumber(outputIndex, number);mInputMemory.markFrameNumber(index, number);
    status = mStreams[0]->bufDone(index);
    if (status) {
        pthread_mutex_lock(&mLock);pending = {};mInputBusy[index] = false;pthread_mutex_unlock(&mLock);
        mMemory.markFrameNumber(outputIndex, -1);mInputMemory.markFrameNumber(index, -1);
        mMemory.unregisterBuffer(outputIndex);return status;
    }
    indexUsed = index;pthread_cond_signal(&mCondition);return 0;
}
void QCamera3VideoEffectsChannel::streamCbRoutine(mm_camera_super_buf_t *frame, QCamera3Stream *stream) {
    if (!mCpuEnabled) { QCamera3RegularChannel::streamCbRoutine(frame, stream);return; }
    if (!frame) return;
    if (!stream || frame->num_bufs != 1 || !frame->bufs[0] || frame->bufs[0]->buf_idx >= kBuffers) {
        free(frame);return;
    }
    mm_camera_buf_def_t *buffer = frame->bufs[0];unsigned index = buffer->buf_idx;
    cam_frame_len_offset_t layout = {};stream->getFrameOffset(layout);
    int32_t cache = mInputMemory.invalidateCache(index);
    pthread_mutex_lock(&mLock);Pending &pending = mPending[index];
    if (!mStopping && pending.active && !pending.imageReady) {
        if (IS_BUFFER_ERROR(buffer->flags) || cache || layout.num_planes != 2 || !buffer->buffer ||
            buffer->frame_len < layout.frame_len || layout.mp[0].stride <= 0 ||
            layout.mp[1].stride <= 0 || layout.mp[0].scanline <= 0 || layout.mp[1].scanline <= 0)
            pending.failed = true;
        else {
            pending.image = {static_cast<uint8_t *>(buffer->buffer), buffer->frame_len,
                mCamera3Stream->width, mCamera3Stream->height, layout.mp[0].offset,
                size_t(layout.mp[0].len) + layout.mp[1].offset, uint32_t(layout.mp[0].stride),
                uint32_t(layout.mp[1].stride), uint32_t(layout.mp[0].scanline),
                uint32_t(layout.mp[1].scanline)};
        }
        pending.imageReady = true;
    } else if (!pending.active && mInputBusy[index]) {
        mInputBusy[index] = false;
        mMemory.unregisterBuffer(index + kBuffers);
    }
    pthread_cond_broadcast(&mCondition);pthread_mutex_unlock(&mLock);free(frame);
}
int32_t QCamera3VideoEffectsChannel::timeoutFrame(uint32_t number) {
    if (!mCpuEnabled) return QCamera3RegularChannel::timeoutFrame(number);
    pthread_mutex_lock(&mLock);
    for (Pending &pending : mPending) if (pending.active && pending.frameNumber == number) pending.failed = true;
    pthread_cond_broadcast(&mCondition);pthread_mutex_unlock(&mLock);return 0;
}
int32_t QCamera3VideoEffectsChannel::output(Pending &pending) {
    const uint32_t width = mCamera3Stream->width, height = mCamera3Stream->height;
    const size_t pixels = size_t(width) * height;
    uint8_t *data = static_cast<uint8_t *>(malloc(pixels * 3 / 2));if (!data) return -ENOMEM;
    QCamera3VideoEffects::Image target = {data, pixels * 3 / 2, width, height, 0, pixels,
        width, width, height, height / 2};
    int32_t status = QCamera3VideoEffects::process(pending.image, false, target, false,
        pending.effects, &mCancelled);
    const int32_t effectStatus = status;
    // A failed stock core cannot expose partial edits. Preserve the base frame
    // for the consumer while retaining the effect failure in buffer status.
    if (status && status != -ECANCELED && !mCancelled.load())
        status = QCamera3VideoEffects::process(pending.image, false, target, false, {}, &mCancelled);
    if (!status && !mCancelled.load()) {
        auto &mapper = android::GraphicBufferMapper::get();android_ycbcr layout = {};
        status = mapper.lockYCbCr(*pending.buffer, GRALLOC_USAGE_SW_WRITE_OFTEN, android::Rect(width, height), &layout);
        if (!status) {
            if (!layout.y || !layout.cb || !layout.cr || !layout.chroma_step || layout.chroma_step > 2 ||
                layout.ystride < width || layout.cstride < (width / 2 - 1) * layout.chroma_step + 1) status = -EINVAL;
            else {
                for (uint32_t row = 0; row < height; ++row)
                    memcpy(static_cast<uint8_t *>(layout.y) + row * layout.ystride, data + row * width, width);
                for (uint32_t row = 0; row < height / 2; ++row) for (uint32_t col = 0; col < width / 2; ++col) {
                    static_cast<uint8_t *>(layout.cb)[row * layout.cstride + col * layout.chroma_step] = data[pixels + row * width + col * 2];
                    static_cast<uint8_t *>(layout.cr)[row * layout.cstride + col * layout.chroma_step] = data[pixels + row * width + col * 2 + 1];
                }
            }
            const int32_t unlock = mapper.unlock(*pending.buffer);if (!status) status = unlock;
        }
    } else if (!status) status = -ECANCELED;
    const int32_t completed = status ? status : effectStatus;
    free(data);return completed;
}

void *QCamera3VideoEffectsChannel::workerEntry(void *user) {
    static_cast<QCamera3VideoEffectsChannel *>(user)->run();return nullptr;
}
void QCamera3VideoEffectsChannel::run() {
    pthread_mutex_lock(&mLock);
    while (!mStopping) {
        unsigned index = kBuffers;
        for (unsigned i = 0; i < kBuffers; ++i)
            if (mPending[i].active && (index == kBuffers || mPending[i].order < mPending[index].order)) index = i;
        if (index < kBuffers) {
            Pending &head = mPending[index];const bool timeout = bootTime() - head.queuedNs >= 500000000;
            if (head.failed || timeout || head.imageReady) {
                Pending job = head;pthread_mutex_unlock(&mLock);
                int32_t status = job.failed ? -EIO : timeout ? -ETIMEDOUT : output(job);
                pthread_mutex_lock(&mLock);if (mStopping) break;
                if (head.failed) status = -ETIMEDOUT;
                buffer_handle_t *buffer = head.buffer;const uint32_t number = head.frameNumber;
                const int outputIndex = head.outputIndex;
                mInputBusy[index] = !head.imageReady;head = {};mMemory.markFrameNumber(outputIndex, -1);
                // Framework registrations are per frame; the private DMA slot
                // remains mapped in mInputMemory. Retain only a timed-out slot
                // whose acquisition callback has not arrived yet.
                if (!mInputBusy[index]) mMemory.unregisterBuffer(outputIndex);
                pthread_mutex_unlock(&mLock);
                issueChannelCb(buffer, number, status ? CAMERA3_BUFFER_STATUS_ERROR : CAMERA3_BUFFER_STATUS_OK);
                pthread_mutex_lock(&mLock);continue;
            }
        }
        struct timespec deadline;clock_gettime(CLOCK_REALTIME, &deadline);deadline.tv_nsec += 10000000;
        if (deadline.tv_nsec >= 1000000000) { ++deadline.tv_sec;deadline.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&mCondition, &mLock, &deadline);
    }
    pthread_mutex_unlock(&mLock);
}
}
