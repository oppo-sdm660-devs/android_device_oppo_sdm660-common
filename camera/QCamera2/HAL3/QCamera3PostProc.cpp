/* Copyright (c) 2012-2019, The Linux Foundation. All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are
* met:
*     * Redistributions of source code must retain the above copyright
*       notice, this list of conditions and the following disclaimer.
*     * Redistributions in binary form must reproduce the above
*       copyright notice, this list of conditions and the following
*       disclaimer in the documentation and/or other materials provided
*       with the distribution.
*     * Neither the name of The Linux Foundation nor the names of its
*       contributors may be used to endorse or promote products derived
*       from this software without specific prior written permission.
*
* THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
* WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
* ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
* BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
* CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
* SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
* BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
* WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
* OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
* IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*
*/

#define LOG_TAG "QCamera3PostProc"

// To remove
#include <cutils/properties.h>

// System dependencies
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <vector>

// Camera dependencies
#include "QCamera3Channel.h"
#include "QCamera3HWI.h"
#include "QCamera3PostProc.h"
#include "QCamera3LowLightHdrAlgo.h"
#include "QCamera3HdrDenoise.h"
#include "QCamera3StillAlgo.h"
#include "QCamera3StockBeauty.h"
#include "QCamera3StockSuperPhoto.h"
#include "QCamera3EisSnapshot.h"
#include "QCamera3FlashSequence.h"
#include "QCamera3SceneBrightness.h"
#include "QCamera3Occlusion.h"
#include "QCamera3PostprocessAssets.h"
#include "QCamera3NativeAlgorithms.h"
#include "QCamera3Stream.h"
#include "QCameraTrace.h"
#include <errno.h>
#include <time.h>

extern "C" {
#include "mm_camera_dbg.h"
}

#define ENABLE_MODEL_INFO_EXIF

namespace qcamera {

static void nativeAlgorithmTrace(jpeg_settings_t *settings, const char *algorithm,
        int32_t status, uint32_t frame, uint32_t inputs,
        QCamera3HardwareInterface *hardware, int32_t applied = 0)
{
    if (!settings) return;
    uint32_t frameworkFrame = settings->framework_frame_number_valid ?
            settings->framework_frame_number : UINT32_MAX;
    // Snapshot the mapping at request submission: long processing may outlive
    // the registry's 256-entry LRU while preview continues to submit frames.
    if ((!settings->framework_frame_number_valid && (!hardware ||
            hardware->getFrameworkFrameNumber(frame, frameworkFrame) != NO_ERROR)) ||
            frameworkFrame == UINT32_MAX) {
        frameworkFrame = UINT32_MAX;
        if (status == NO_ERROR) status = -ENOENT;
    }
    // Written after processing, never inferred from the requested mode. A base
    // JPEG returned after a core failure remains distinguishable from success.
    snprintf(settings->image_desc, sizeof(settings->image_desc),
            "OPPO_HAL3 algorithm=%s status=%d frame=%u inputs=%u",
            algorithm, status, frameworkFrame, inputs);
    settings->image_desc_valid = 1;
    // EXIF ImageDescription is only EXIF_IMAGE_DESCRIPTION_SIZE bytes, so the
    // joined multi-stage trace is carried separately and published as the
    // nativeTrace result key. A stage is appended only when it really ran.
    if (settings->native_trace.add(algorithm, status, frameworkFrame, inputs)) {
        settings->native_trace_valid = 1;
        if (hardware) {
            hardware->noteNativeTrace(frame, settings->native_trace.c_str(),
                    status == NO_ERROR ? settings->native_still_mode : OPPO_NATIVE_OFF);
            // Per-frame diagnostics record of the same really-executed stage.
            hardware->noteNativeStage(frame, settings->native_still_mode, status, applied);
        }
    }
    LOGI("%s", settings->image_desc);
}

static const char ExifAsciiPrefix[] =
    { 0x41, 0x53, 0x43, 0x49, 0x49, 0x0, 0x0, 0x0 };          // "ASCII\0\0\0"

__unused
static const char ExifUndefinedPrefix[] =
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };   // "\0\0\0\0\0\0\0\0"

#define EXIF_ASCII_PREFIX_SIZE           8   //(sizeof(ExifAsciiPrefix))
#define FOCAL_LENGTH_DECIMAL_PRECISION   1000

/*===========================================================================
 * FUNCTION   : QCamera3PostProcessor
 *
 * DESCRIPTION: constructor of QCamera3PostProcessor.
 *
 * PARAMETERS :
 *   @cam_ctrl : ptr to HWI object
 *
 * RETURN     : None
 *==========================================================================*/
QCamera3PostProcessor::QCamera3PostProcessor(QCamera3ProcessingChannel* ch_ctrl)
    : m_parent(ch_ctrl),
      mJpegCB(NULL),
      mJpegUserData(NULL),
      mJpegClientHandle(0),
      mJpegSessionId(0),
      m_bThumbnailNeeded(TRUE),
      mOutputMem(NULL),
      mDeinitialized(false),
      mStopCommandQueued(false),
      mLifecycleOperationActive(false),
      mStopDeferred(false),
      m_ppChannelCnt(1),
      m_bMpoEnabled(FALSE),
      m_inputPPQ(releasePPInputData, this),
      m_inputFWKPPQ(NULL, this),
      m_inputMultiReprocQ(releaseOngoingPPData, this),
      m_ongoingPPQ(releaseOngoingPPData, this),
      m_inputJpegQ(releaseJpegData, this),
      m_ongoingJpegQ(releaseJpegData, this),
      m_inputMetaQ(releaseMetadata, this),
      m_jpegSettingsQ(NULL, this),
      mChannelStop(TRUE)
{
    memset(&mJpegHandle, 0, sizeof(mJpegHandle));
    memset(&mJpegMetadata, 0, sizeof(mJpegMetadata));
    memset(m_pReprocChannel, 0, sizeof(m_pReprocChannel));
    mReprocessNode.clear();
    pthread_mutex_init(&mReprocJobLock, NULL);
    pthread_mutex_init(&mCancelJobLock, NULL);
    pthread_mutex_init(&mHDRJobLock, NULL);
    pthread_mutexattr_t hdrLockAttr;
    pthread_mutexattr_init(&hdrLockAttr);
    pthread_mutexattr_settype(&hdrLockAttr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&mHdrCaptureLock, &hdrLockAttr);
    pthread_mutexattr_destroy(&hdrLockAttr);
    pthread_cond_init(&mHdrCaptureCond, NULL);
    mHdrActive = false;
    mHdrBaseJob = NULL;
    mHdrBaseFrame = NULL;
    memset(mHdrImages, 0, sizeof(mHdrImages));
    pthread_cond_init(&mProcChStopCond, NULL);
    max_pic_size = {0,0};
    mCancelpprocReqFrameList.clear();
}

/*===========================================================================
 * FUNCTION   : ~QCamera3PostProcessor
 *
 * DESCRIPTION: deconstructor of QCamera3PostProcessor.
 *
 * PARAMETERS : None
 *
 * RETURN     : None
 *==========================================================================*/
QCamera3PostProcessor::~QCamera3PostProcessor()
{
    // The owning channel calls this before its derived buffers disappear.
    // Keep the fallback drain ahead of mutex and queue destruction as well.
    deinit();

    pthread_mutex_destroy(&mReprocJobLock);
    pthread_mutex_destroy(&mCancelJobLock);
    pthread_mutex_destroy(&mHDRJobLock);
    pthread_mutex_destroy(&mHdrCaptureLock);
    pthread_cond_destroy(&mHdrCaptureCond);
    pthread_cond_destroy(&mProcChStopCond);
}

/*===========================================================================
 * FUNCTION   : init
 *
 * DESCRIPTION: initialization of postprocessor
 *
 * PARAMETERS :
 *   @memory              : output buffer memory
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t QCamera3PostProcessor::init(QCamera3StreamMem *memory)
{
    std::lock_guard<std::mutex> lifecycle(mLifecycleLock);
    ATRACE_CAMSCOPE_CALL(CAMSCOPE_HAL3_PPROC_INIT);
    mOutputMem = memory;
    mDeinitialized = false;
    int32_t rc = m_dataProcTh.launch(dataProcessRoutine, this);
    mFreeJpegSessions.clear();
    return rc;
}

/*===========================================================================
 * FUNCTION   : deinit
 *
 * DESCRIPTION: de-initialization of postprocessor
 *
 * PARAMETERS : None
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t QCamera3PostProcessor::deinit()
{
    std::lock_guard<std::mutex> lifecycle(mLifecycleLock);
    if (mDeinitialized.load()) return NO_ERROR;
    mDeinitialized = true;
    pthread_mutex_lock(&mHDRJobLock);
    pthread_cond_broadcast(&mProcChStopCond);
    pthread_mutex_unlock(&mHDRJobLock);
    int rc = NO_ERROR;
    abortHdrCapture();
    // Stop submission first, then join driver callbacks while channel objects
    // and their source/CPP memory are still available to the queue releasers.
    bool workerPaused = m_dataProcTh.sendCmd(
            CAMERA_CMD_TYPE_PAUSE_DATA_PROC, TRUE, TRUE) == NO_ERROR;
    bool workerStopped = workerPaused && m_dataProcTh.sendCmd(
            CAMERA_CMD_TYPE_STOP_DATA_PROC, TRUE, TRUE) == NO_ERROR;
    m_dataProcTh.exit();
    if (!workerStopped) stopPostproc();
    // Also handles an unlaunched worker or a failed STOP allocation. Closing
    // admission and draining again captures callbacks that raced shutdown.
    clearPostprocQueues();

    if(mJpegClientHandle > 0) {
        rc = mJpegHandle.close(mJpegClientHandle);
        LOGH("Jpeg closed, rc = %d, mJpegClientHandle = %x",
               rc, mJpegClientHandle);
        mJpegClientHandle = 0;
        memset(&mJpegHandle, 0, sizeof(mJpegHandle));
    }

    mOutputMem = NULL;
    return rc;
}

/*===========================================================================
 * FUNCTION   : initJpeg
 *
 * DESCRIPTION: initialization of jpeg through postprocessor
 *
 * PARAMETERS :
 *   @jpeg_cb      : callback to handle jpeg event from mm-camera-interface
 *   @max_pic_dim  : max picture dimensions
 *   @user_data    : user data ptr for jpeg callback
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t QCamera3PostProcessor::initJpeg(jpeg_encode_callback_t jpeg_cb,
                                            mpo_encode_callback_t mpo_cb,
                                            cam_dimension_t* max_pic_dim,
                                            void *user_data)
{
    ATRACE_CAMSCOPE_CALL(CAMSCOPE_HAL3_PPROC_INIT_JPEG);
    mJpegCB = jpeg_cb;
    mMpoCB = mpo_cb;
    mJpegUserData = user_data;

    mMpoInputData.clear();

    if ((0 > max_pic_dim->width) || (0 > max_pic_dim->height)) {
        LOGE("Negative dimension %dx%d",
                max_pic_dim->width, max_pic_dim->height);
        return BAD_VALUE;
    }

    // set max pic size
    memset(&max_pic_size, 0, sizeof(mm_dimension));
    max_pic_size.w =  max_pic_dim->width;
    max_pic_size.h =  max_pic_dim->height;

    // Pass OTP calibration data to JPEG
    QCamera3HardwareInterface* hal_obj = (QCamera3HardwareInterface*)m_parent->mUserData;
    mJpegMetadata.default_sensor_flip = FLIP_NONE;
    mJpegMetadata.sensor_mount_angle = hal_obj->getSensorMountAngle();
    memcpy(&mJpegMetadata.otp_calibration_data,
            hal_obj->getRelatedCalibrationData(),
            sizeof(cam_related_system_calibration_data_t));
    m_dataProcTh.sendCmd(CAMERA_CMD_TYPE_INIT_JPEG, FALSE, TRUE);
    return NO_ERROR;
}

/*===========================================================================
 * FUNCTION   : start
 *
 * DESCRIPTION: start postprocessor. Data process thread and data notify thread
 *              will be launched.
 *
 * PARAMETERS :
 *   @config        : reprocess configuration
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *
 * NOTE       : if any reprocess is needed, a reprocess channel/stream
 *              will be started.
 *==========================================================================*/
int32_t QCamera3PostProcessor::start(const reprocess_config_t &config)
{
    int32_t rc = NO_ERROR;
    std::unique_lock<std::mutex> lifecycle(mLifecycleLock, std::defer_lock);
    for (;;) {
        // A callback marks STOP pending before releasing its framework result.
        // Do not hold the lifecycle lock while waiting for that callback to
        // enqueue STOP or for the worker to complete its cleanup.
        pthread_mutex_lock(&mHDRJobLock);
        while (!mChannelStop && !mDeinitialized.load())
            pthread_cond_wait(&mProcChStopCond, &mHDRJobLock);
        pthread_mutex_unlock(&mHDRJobLock);
        lifecycle.lock();
        if (mDeinitialized.load()) return -ENODEV;
        pthread_mutex_lock(&mHDRJobLock);
        const bool stopped = mChannelStop;
        if (stopped) mLifecycleOperationActive = true;
        pthread_mutex_unlock(&mHDRJobLock);
        if (stopped) break;
        lifecycle.unlock();
    }
    auto finishStart = [this](QCamera3PostProcessor *) { finishLifecycleOperation(); };
    std::unique_ptr<QCamera3PostProcessor, decltype(finishStart)> startGuard(this, finishStart);
    QCamera3HardwareInterface* hal_obj = (QCamera3HardwareInterface*)m_parent->mUserData;
    if (config.reprocess_type != REPROCESS_TYPE_NONE) {
        for (int8_t i = 0; i < m_ppChannelCnt; i++) {
            QCamera3Channel *pChannel = m_pReprocChannel[i];
            if (pChannel != NULL ) {
                pChannel->stop();
                delete pChannel;
                m_pReprocChannel[i] = NULL;
            }
        }
        m_ppChannelCnt = 0;

       //if YUV support quadra remosaic and 2 yuv configured/requested one with quadra size and
       //other with offline reprocessing. then need to differentiate for how many reprocess
       //channel required for which YUV channel.
       if((config.reprocess_type == REPROCESS_TYPE_YUV)
            && hal_obj->m_bQuadraCfaRequest
            && !m_parent->m_bQuadraChannel)
       {
           m_ppChannelCnt = 1; //setting 1 for non-quadra YUV at remosaic requested PCR from framework.
       } else {
            m_ppChannelCnt = hal_obj->getReprocChannelCnt();
       }

        LOGH("m_ppChannelCnt:%d", m_ppChannelCnt);

        reprocess_config_t local_cfg = config;
        for (int8_t i = 0; i < m_ppChannelCnt; i++) {
            LOGD("src channel:%p, input channel:%p", local_cfg.src_channel, m_parent);
            m_pReprocChannel[i] = hal_obj->addOfflineReprocChannel(local_cfg, m_parent, i);
            if (m_pReprocChannel[i] == NULL) {
                LOGE("cannot add reprocess channel, idx:%d", i);
                return UNKNOWN_ERROR;
            }
            local_cfg.src_channel = (QCamera3Channel *)m_pReprocChannel[i];
        }

        /*start the reprocess channel only if buffers are already allocated, thus
          only start it in an intermediate reprocess type, defer it for others*/
        if ((config.reprocess_type == REPROCESS_TYPE_JPEG)
            || ((config.reprocess_type == REPROCESS_TYPE_YUV) && (m_parent->m_bQuadraChannel))) {
            for (int8_t i = 0; i < m_ppChannelCnt; i++) {
                rc = m_pReprocChannel[i]->start();
                if (rc != 0) {
                    LOGE("cannot start reprocess channel, idx:%d", i);
                    delete m_pReprocChannel[i];
                    m_pReprocChannel[i] = NULL;
                    return rc;
                }
            }
        }
    }
    rc = m_dataProcTh.sendCmd(CAMERA_CMD_TYPE_START_DATA_PROC, TRUE, FALSE);
    startGuard.release();
    const bool stoppedDuringStart = finishLifecycleOperation();
    return rc != NO_ERROR ? rc : (stoppedDuringStart ? -ECANCELED : NO_ERROR);
}

bool QCamera3PostProcessor::finishLifecycleOperation()
{
    pthread_mutex_lock(&mHDRJobLock);
    mLifecycleOperationActive = false;
    const bool cancelled = !mChannelStop;
    const bool deferred = mStopDeferred;
    mStopDeferred = false;
    if (deferred) mStopCommandQueued = true;
    pthread_mutex_unlock(&mHDRJobLock);
    if (deferred) {
        // The lifecycle operation has completed before this priority STOP can
        // be consumed, including START's synchronous acknowledgement.
        // Async STOP is allocation independent and cannot wait on callbacks.
        int32_t rc = m_dataProcTh.sendCmd(CAMERA_CMD_TYPE_STOP_DATA_PROC, FALSE, TRUE);
        if (rc != NO_ERROR) {
            pthread_mutex_lock(&mHDRJobLock);
            mStopCommandQueued = false;
            mChannelStop = true;
            pthread_cond_broadcast(&mProcChStopCond);
            pthread_mutex_unlock(&mHDRJobLock);
        }
    }
    return cancelled;
}

/*===========================================================================
 * FUNCTION   : flush
 *
 * DESCRIPTION: stop ongoing postprocess and jpeg jobs
 *
 * PARAMETERS : None
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *
 *==========================================================================*/
int32_t QCamera3PostProcessor::flush()
{
    std::unique_lock<std::mutex> lifecycle(mLifecycleLock, std::defer_lock);
    for (;;) {
        pthread_mutex_lock(&mHDRJobLock);
        while (!mChannelStop && !mDeinitialized.load())
            pthread_cond_wait(&mProcChStopCond, &mHDRJobLock);
        pthread_mutex_unlock(&mHDRJobLock);
        lifecycle.lock();
        if (mDeinitialized.load()) return -ENODEV;
        pthread_mutex_lock(&mHDRJobLock);
        const bool stopped = mChannelStop;
        if (stopped) mLifecycleOperationActive = true;
        pthread_mutex_unlock(&mHDRJobLock);
        if (stopped) break;
        lifecycle.unlock();
    }
    auto finishFlush = [this](QCamera3PostProcessor *) { finishLifecycleOperation(); };
    std::unique_ptr<QCamera3PostProcessor, decltype(finishFlush)> flushGuard(this, finishFlush);
    abortHdrCapture();
    int32_t rc = NO_ERROR;
    qcamera_hal3_jpeg_data_t *jpeg_job =
            (qcamera_hal3_jpeg_data_t *)m_ongoingJpegQ.dequeue();
    while (jpeg_job != NULL) {
        rc = mJpegHandle.abort_job(jpeg_job->jobId);
        releaseJpegData(jpeg_job, this);
        free(jpeg_job);

        jpeg_job = (qcamera_hal3_jpeg_data_t *)m_ongoingJpegQ.dequeue();
    }
    rc = releaseOfflineBuffers(true);
    return rc;
}

/*===========================================================================
 * FUNCTION   : stop
 *
 * DESCRIPTION: stop postprocessor. Data process and notify thread will be stopped.
 *
 * PARAMETERS : None
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *
 * NOTE       : reprocess channel will be stopped and deleted if there is any
 *==========================================================================*/
int32_t QCamera3PostProcessor::stop(bool isHDR)
{
    // Async STOP comes from the JPEG callback. It must never wait for the
    // lifecycle lock held by deinit/STOP while those operations join callbacks.
    std::unique_lock<std::mutex> lifecycle(mLifecycleLock, std::defer_lock);
    if (!isHDR) lifecycle.lock();
    if (mDeinitialized.load()) return NO_ERROR;
    abortHdrCapture();
    pthread_mutex_lock(&mHDRJobLock);
    if (isHDR && mLifecycleOperationActive) {
        mStopDeferred = true;
        mChannelStop = false;
        pthread_mutex_unlock(&mHDRJobLock);
        return NO_ERROR;
    }
    if (mStopCommandQueued) {
        if (!isHDR) {
            while (mStopCommandQueued)
                pthread_cond_wait(&mProcChStopCond, &mHDRJobLock);
        }
        pthread_mutex_unlock(&mHDRJobLock);
        return NO_ERROR;
    }
    mStopCommandQueued = true;
    mChannelStop = false;
    pthread_mutex_unlock(&mHDRJobLock);
    int32_t rc = m_dataProcTh.sendCmd(CAMERA_CMD_TYPE_STOP_DATA_PROC, !isHDR, TRUE);
    if (rc != NO_ERROR) {
        if (isHDR) {
            // Async STOP survives allocation failure in QCameraCmdThread.
            // An unlaunched/exiting worker leaves cleanup to deinit.
            pthread_mutex_lock(&mHDRJobLock);
            mStopCommandQueued = false;
            mChannelStop = true;
            pthread_cond_broadcast(&mProcChStopCond);
            pthread_mutex_unlock(&mHDRJobLock);
            return rc;
        }
        // No worker can drain an unaccepted STOP. Join it before the fallback.
        m_dataProcTh.exit();
        stopPostproc();
    }
    return rc;
}

void QCamera3PostProcessor::markStopPending()
{
    pthread_mutex_lock(&mHDRJobLock);
    if (!mDeinitialized.load()) mChannelStop = false;
    pthread_mutex_unlock(&mHDRJobLock);
}

void QCamera3PostProcessor::stopPostproc()
{
    for (int8_t i = 0; i < m_ppChannelCnt; i++) {
        QCamera3Channel *pChannel = m_pReprocChannel[i];
        if (pChannel != NULL) pChannel->stop();
    }
    clearPostprocQueues();
    // Queued owners may return CPP slots through these channel objects.
    // Keep them alive until every releaser has completed.
    for (int8_t i = 0; i < m_ppChannelCnt; i++) {
        delete m_pReprocChannel[i];
        m_pReprocChannel[i] = NULL;
    }
    m_ppChannelCnt = 0;
    pthread_mutex_lock(&mCancelJobLock);
    mCancelpprocReqFrameList.clear();
    pthread_mutex_unlock(&mCancelJobLock);
    pthread_mutex_lock(&mHdrCaptureLock);
    mHdrFrameStates.clear();
    mHdrDiscardFrames.clear();
    mHdrFailedFrames.clear();
    pthread_mutex_unlock(&mHdrCaptureLock);

    pthread_mutex_lock(&mHDRJobLock);
    mStopCommandQueued = false;
    mChannelStop = true;
    pthread_cond_broadcast(&mProcChStopCond);
    pthread_mutex_unlock(&mHDRJobLock);
}

/*===========================================================================
 * FUNCTION   : getFWKJpegEncodeConfig
 *
 * DESCRIPTION: function to prepare encoding job information
 *
 * PARAMETERS :
 *   @encode_parm   : param to be filled with encoding configuration
 *   @frame         : framework input buffer
 *   @jpeg_settings : jpeg settings to be applied for encoding
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t QCamera3PostProcessor::getFWKJpegEncodeConfig(
        mm_jpeg_encode_params_t& encode_parm,
        qcamera_fwk_input_pp_data_t *frame,
        jpeg_settings_t *jpeg_settings)
{
    LOGD("E");
    int32_t ret = NO_ERROR;

    if ((NULL == frame) || (NULL == jpeg_settings)) {
        return BAD_VALUE;
    }

    ssize_t bufSize = mOutputMem->getSize(jpeg_settings->out_buf_index);
    if (BAD_INDEX == bufSize) {
        LOGE("cannot retrieve buffer size for buffer %u",
                jpeg_settings->out_buf_index);
        return BAD_VALUE;
    }

    encode_parm.jpeg_cb = mJpegCB;
    encode_parm.userdata = mJpegUserData;

    if (jpeg_settings->thumbnail_size.width > 0 &&
            jpeg_settings->thumbnail_size.height > 0)
        m_bThumbnailNeeded = TRUE;
    else
        m_bThumbnailNeeded = FALSE;
    encode_parm.encode_thumbnail = m_bThumbnailNeeded;

    // get color format
    cam_format_t img_fmt = frame->reproc_config.stream_format;
    encode_parm.color_format = getColorfmtFromImgFmt(img_fmt);

    // get jpeg quality
    encode_parm.quality = jpeg_settings->jpeg_quality;
    if (encode_parm.quality <= 0) {
        encode_parm.quality = 85;
    }

    // get jpeg thumbnail quality
    encode_parm.thumb_quality = jpeg_settings->jpeg_thumb_quality;

    cam_frame_len_offset_t main_offset =
            frame->reproc_config.input_stream_plane_info.plane_info;

    encode_parm.num_src_bufs = 1;
    encode_parm.src_main_buf[0].index = 0;
    encode_parm.src_main_buf[0].buf_size = frame->input_buffer.frame_len;
    encode_parm.src_main_buf[0].buf_vaddr = (uint8_t *) frame->input_buffer.buffer;
    encode_parm.src_main_buf[0].fd = frame->input_buffer.fd;
    encode_parm.src_main_buf[0].format = MM_JPEG_FMT_YUV;
    encode_parm.src_main_buf[0].offset = main_offset;

    //Pass input thumbnail buffer info to encoder.
    //Note: Use main buffer to encode thumbnail
    if (m_bThumbnailNeeded == TRUE) {
        encode_parm.num_tmb_bufs = 1;
        encode_parm.src_thumb_buf[0] = encode_parm.src_main_buf[0];
    }

    //Pass output jpeg buffer info to encoder.
    //mOutputMem is allocated by framework.
    encode_parm.num_dst_bufs = 1;
    encode_parm.dest_buf[0].index = 0;
    encode_parm.dest_buf[0].buf_size = (size_t)bufSize;
    encode_parm.dest_buf[0].buf_vaddr = (uint8_t *)mOutputMem->getPtr(
            jpeg_settings->out_buf_index);
    encode_parm.dest_buf[0].fd = mOutputMem->getFd(
            jpeg_settings->out_buf_index);
    encode_parm.dest_buf[0].format = MM_JPEG_FMT_YUV;
    encode_parm.dest_buf[0].offset = main_offset;

    LOGD("X");
    return NO_ERROR;

    LOGD("X with error %d", ret);
    return ret;
}

/*===========================================================================
 * FUNCTION   : getJpegEncodeConfig
 *
 * DESCRIPTION: function to prepare encoding job information
 *
 * PARAMETERS :
 *   @encode_parm   : param to be filled with encoding configuration
 *   #main_stream   : stream object where the input buffer comes from
 *   @jpeg_settings : jpeg settings to be applied for encoding
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t QCamera3PostProcessor::getJpegEncodeConfig(
                mm_jpeg_encode_params_t& encode_parm,
                QCamera3Stream *main_stream,
                jpeg_settings_t *jpeg_settings,
                mm_camera_buf_def_t *input_buf)
{
    LOGD("E");
    int32_t ret = NO_ERROR;
    ssize_t bufSize = 0;
    QCamera3StreamMem *outputMem = mOutputMem;

    if(jpeg_settings->encode_type == MM_JPEG_TYPE_JPEG)
    {
        encode_parm.jpeg_cb = mJpegCB;
        encode_parm.userdata = mJpegUserData;
    } else {
        // MPO usecase.
        encode_parm.jpeg_cb = processJpegData;
        encode_parm.userdata = this;
        outputMem = &m_parent->getJpegMemory();
    }

    if (jpeg_settings->thumbnail_size.width > 0 &&
            jpeg_settings->thumbnail_size.height > 0)
        m_bThumbnailNeeded = TRUE;
    else
        m_bThumbnailNeeded = FALSE;
    encode_parm.encode_thumbnail = m_bThumbnailNeeded;

    // get color format
    cam_format_t img_fmt = CAM_FORMAT_YUV_420_NV12;  //default value
    if(jpeg_settings->is_format_valid)
    {
        img_fmt = jpeg_settings->format;
    } else {
        if(main_stream->getFormat(img_fmt) < 0)
        {
            LOGE("Error: failed to get image format");
        }
    }
    encode_parm.color_format = getColorfmtFromImgFmt(img_fmt);

    // get jpeg quality
    encode_parm.quality = jpeg_settings->jpeg_quality;
    if (encode_parm.quality <= 0) {
        encode_parm.quality = 85;
    }

    // get jpeg thumbnail quality
    encode_parm.thumb_quality = jpeg_settings->jpeg_thumb_quality;

    cam_frame_len_offset_t main_offset;
    memset(&main_offset, 0, sizeof(cam_frame_len_offset_t));
    if(jpeg_settings->is_offset_valid)
    {
        main_offset = jpeg_settings->offset;
    } else {
        main_stream->getFrameOffset(main_offset);
    }

    // src buf config
    //Pass input main image buffer info to encoder.
    QCamera3StreamMem *pStreamMem = main_stream->getStreamBufs();
    if (pStreamMem == NULL) {
        LOGE("cannot get stream bufs from main stream");
        ret = BAD_VALUE;
        goto on_error;
    }
    if(input_buf == NULL)
    {
        encode_parm.num_src_bufs = MIN(pStreamMem->getCnt(), MM_JPEG_MAX_BUF);
        for (uint32_t i = 0; i < encode_parm.num_src_bufs; i++) {
            if (pStreamMem != NULL) {
                encode_parm.src_main_buf[i].index = i;
                bufSize = pStreamMem->getSize(i);
                if (BAD_INDEX == bufSize) {
                    LOGE("cannot retrieve buffer size for buffer %u", i);
                    ret = BAD_VALUE;
                    goto on_error;
                }
                encode_parm.src_main_buf[i].buf_size = (size_t)bufSize;
                encode_parm.src_main_buf[i].buf_vaddr = (uint8_t *)pStreamMem->getPtr(i);
                encode_parm.src_main_buf[i].fd = pStreamMem->getFd(i);
                encode_parm.src_main_buf[i].format = MM_JPEG_FMT_YUV;
                encode_parm.src_main_buf[i].offset = main_offset;
            }
        }
    } else {
        encode_parm.num_src_bufs = 1;
        encode_parm.src_main_buf[0].index = 0;
        encode_parm.src_main_buf[0].buf_size = input_buf->frame_len;
        encode_parm.src_main_buf[0].buf_vaddr = (uint8_t *)input_buf->buffer;
        encode_parm.src_main_buf[0].fd = input_buf->fd;
        encode_parm.src_main_buf[0].format = MM_JPEG_FMT_YUV;
        encode_parm.src_main_buf[0].offset = main_offset;
    }

    //Pass input thumbnail buffer info to encoder.
    //Note: Use main buffer to encode thumbnail
    if (m_bThumbnailNeeded == TRUE) {
        pStreamMem = main_stream->getStreamBufs();
        if (pStreamMem == NULL) {
            LOGE("cannot get stream bufs from thumb stream");
            ret = BAD_VALUE;
            goto on_error;
        }
        cam_frame_len_offset_t thumb_offset;
        memset(&thumb_offset, 0, sizeof(cam_frame_len_offset_t));
        main_stream->getFrameOffset(thumb_offset);
        encode_parm.num_tmb_bufs = MIN(pStreamMem->getCnt(), MM_JPEG_MAX_BUF);
        for (uint32_t i = 0; i < encode_parm.num_tmb_bufs; i++) {
            if (pStreamMem != NULL) {
                encode_parm.src_thumb_buf[i].index = i;
                bufSize = pStreamMem->getSize(i);
                if (BAD_INDEX == bufSize) {
                    LOGE("cannot retrieve buffer size for buffer %u", i);
                    ret = BAD_VALUE;
                    goto on_error;
                }
                encode_parm.src_thumb_buf[i].buf_size = (uint32_t)bufSize;
                encode_parm.src_thumb_buf[i].buf_vaddr = (uint8_t *)pStreamMem->getPtr(i);
                encode_parm.src_thumb_buf[i].fd = pStreamMem->getFd(i);
                encode_parm.src_thumb_buf[i].format = MM_JPEG_FMT_YUV;
                encode_parm.src_thumb_buf[i].offset = thumb_offset;
            }
        }
    }

    //Pass output jpeg buffer info to encoder.
    //mJpegMem is allocated by framework.
    bufSize = outputMem->getSize(jpeg_settings->out_buf_index);
    if (BAD_INDEX == bufSize) {
        LOGE("cannot retrieve buffer size for buffer %u",
                jpeg_settings->out_buf_index);
        ret = BAD_VALUE;
        goto on_error;
    }

    encode_parm.num_dst_bufs = 1;
    encode_parm.dest_buf[0].index = 0;
    encode_parm.dest_buf[0].buf_size = (size_t)bufSize;
    encode_parm.dest_buf[0].buf_vaddr = (uint8_t *)outputMem->getPtr(
            jpeg_settings->out_buf_index);
    encode_parm.dest_buf[0].fd = outputMem->getFd(
            jpeg_settings->out_buf_index);
    encode_parm.dest_buf[0].format = MM_JPEG_FMT_YUV;
    encode_parm.dest_buf[0].offset = main_offset;

    LOGD("X");
    return NO_ERROR;

on_error:
    LOGD("X with error %d", ret);
    return ret;
}

int32_t QCamera3PostProcessor::processData(mm_camera_super_buf_t *input) {
    return processData(input, NULL, 0);
}

/*===========================================================================
 * FUNCTION   : processData
 *
 * DESCRIPTION: enqueue data into dataProc thread
 *
 * PARAMETERS :
 *   @frame   : process input frame
 *   @output  : process output frame
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *
 * NOTE       : depends on if offline reprocess is needed, received frame will
 *              be sent to either input queue of postprocess or jpeg encoding
 *==========================================================================*/
int32_t QCamera3PostProcessor::processData(mm_camera_super_buf_t *input,
        buffer_handle_t *output, uint32_t frameNumber)
{
    LOGD("E");
    pthread_mutex_lock(&mReprocJobLock);

    // enqueue to post proc input queue
    qcamera_hal3_pp_buffer_t *pp_buffer = (qcamera_hal3_pp_buffer_t *)malloc(
            sizeof(qcamera_hal3_pp_buffer_t));
    if (NULL == pp_buffer) {
        LOGE("out of memory");
        pthread_mutex_unlock(&mReprocJobLock);
        return NO_MEMORY;
    }
    memset(pp_buffer, 0, sizeof(*pp_buffer));
    pp_buffer->input = input;
    pp_buffer->output = output;
    pp_buffer->frameNumber = frameNumber;
    const bool cancelled = isQueuedFrameCancelled(frameNumber);
    bool queued = true;
    if (cancelled) {
        queued = m_inputPPQ.enqueue((void *)pp_buffer);
        m_dataProcTh.sendCmd(CAMERA_CMD_TYPE_CANCEL_PP_FRAME, FALSE, FALSE);
    } else if (!(m_inputMetaQ.isEmpty())) {
        qcamera_hal3_meta_pp_buffer_t *meta_job = isMetaMatched(frameNumber);
        if(meta_job != NULL) {
            ReprocessBuffer reproc;
            reproc.metaBuffer = meta_job;
            reproc.reprocBuf = pp_buffer;
            mReprocessNode.push_back(reproc);
            LOGD("meta queue is not empty, do next job");
            m_dataProcTh.sendCmd(CAMERA_CMD_TYPE_DO_NEXT_JOB, FALSE, FALSE);
        } else {
            queued = m_inputPPQ.enqueue((void *)pp_buffer);
        }
    } else {
        LOGD("metadata queue is empty");
        queued = m_inputPPQ.enqueue((void *)pp_buffer);
    }
    pthread_mutex_unlock(&mReprocJobLock);
    if (!queued) {
        releasePPInputData(pp_buffer, this);
        free(pp_buffer);
        return -ECANCELED;
    }
    return NO_ERROR;
}


/*===========================================================================
 * FUNCTION   : isMetaMatched
 *
 * DESCRIPTION: corresponding meta detection
 *
 * PARAMETERS :
 *   @frame   : frame number
 *
 * RETURN     :
 *  TRUE if Frame is present
 *  FALSE if Frame is not released.
 *
 *==========================================================================*/
qcamera_hal3_meta_pp_buffer_t* QCamera3PostProcessor::isMetaMatched(uint32_t resultFrameNumber)
{
    qcamera_hal3_meta_pp_buffer_t *meta_job =
            (qcamera_hal3_meta_pp_buffer_t *)
            m_inputMetaQ.dequeue(matchMetaFrameNum, (void*)&resultFrameNumber);
    if(meta_job != NULL) {
        return meta_job;
    }
    return NULL;
}


/*===========================================================================
 * FUNCTION   : releaseReprocMetaBuffer
 *
 * DESCRIPTION: Release reprocessed Meta.
 *
 * PARAMETERS :
*   @frame   : meta frame number
 *
 * RETURN     :
 *  TRUE if Meta is released.
 *  FALSE if Meta is not released.
 *
 *==========================================================================*/
bool QCamera3PostProcessor::releaseReprocMetaBuffer(uint32_t resultFrameNumber)
{
    pthread_mutex_lock(&mReprocJobLock);
    if (!(m_inputMetaQ.isEmpty())) {
        mm_camera_super_buf_t *meta_buffer = NULL;
        qcamera_hal3_meta_pp_buffer_t *meta_job =
                (qcamera_hal3_meta_pp_buffer_t *)
                m_inputMetaQ.dequeue(matchMetaFrameNum, (void*)&resultFrameNumber);
        if(meta_job != NULL) {
            meta_buffer = meta_job->metabuf;
            m_parent->metadataBufDone(meta_buffer);
            free(meta_job);
            pthread_mutex_unlock(&mReprocJobLock);
            return true;
        }
    }
    pthread_mutex_unlock(&mReprocJobLock);
    return false;
}


bool QCamera3PostProcessor::matchMetaFrameNum(void *data, void *, void *match_data)
{
    qcamera_hal3_meta_pp_buffer_t *job = (qcamera_hal3_meta_pp_buffer_t *) data;
    uint32_t frame_num = *((uint32_t *) match_data);
    LOGD(" Matching MetaFrameNum :%d and %d", frame_num, job->metaFrameNumber);
    return job->metaFrameNumber == frame_num;
}


bool QCamera3PostProcessor::matchReprocessFrameNum(void *data, void *, void *match_data)
{
    qcamera_hal3_pp_buffer_t *job = (qcamera_hal3_pp_buffer_t *) data;
    uint32_t frame_num = *((uint32_t *) match_data);
    LOGD(" Matching FrameNum :%d and %d",frame_num,job->frameNumber);
    return job->frameNumber == frame_num;
}

bool QCamera3PostProcessor::matchJpegSetting(void *data, void *, void *match_data)
{
    jpeg_settings_t* jpeg_setting = (jpeg_settings_t *) data;
    uint32_t frame_num = *((uint32_t *) match_data);
    LOGD(" Matching FrameNum :%d and %d",frame_num,jpeg_setting->frame_number);
    return jpeg_setting->frame_number == frame_num;
}

/*===========================================================================
 * FUNCTION   : needsReprocess
 *
 * DESCRIPTION: Determine if reprocess is needed.
 *
 * PARAMETERS :
 *   @frame   : process frame
 *
 * RETURN     :
 *  TRUE if frame needs to be reprocessed
 *  FALSE is frame does not need to be reprocessed
 *
 *==========================================================================*/
bool QCamera3PostProcessor::needsReprocess(qcamera_fwk_input_pp_data_t *frame)
{
    metadata_buffer_t* meta = (metadata_buffer_t *) frame->metadata_buffer.buffer;
    bool edgeModeOn = FALSE;
    bool noiseRedModeOn = FALSE;
    bool reproNotDone = TRUE;

    if (frame->reproc_config.reprocess_type == REPROCESS_TYPE_NONE) {
        return FALSE;
    }

    // edge detection
    IF_META_AVAILABLE(cam_edge_application_t, edgeMode,
            CAM_INTF_META_EDGE_MODE, meta) {
        edgeModeOn = (CAM_EDGE_MODE_OFF != edgeMode->edge_mode);
    }

    // noise reduction
    IF_META_AVAILABLE(uint32_t, noiseRedMode,
            CAM_INTF_META_NOISE_REDUCTION_MODE, meta) {
        noiseRedModeOn = (CAM_NOISE_REDUCTION_MODE_OFF != *noiseRedMode);
    }

    IF_META_AVAILABLE(uint8_t, reprocess_flags,
            CAM_INTF_META_REPROCESS_FLAGS, meta) {
        reproNotDone = FALSE;
    }

    return (edgeModeOn || noiseRedModeOn || reproNotDone);
}

/*===========================================================================
 * FUNCTION   : processData
 *
 * DESCRIPTION: enqueue data into dataProc thread
 *
 * PARAMETERS :
 *   @frame   : process frame
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *
 * NOTE       : depends on if offline reprocess is needed, received frame will
 *              be sent to either input queue of postprocess or jpeg encoding
 *==========================================================================*/
int32_t QCamera3PostProcessor::processData(qcamera_fwk_input_pp_data_t *frame)
{
    if (needsReprocess(frame)) {
        ATRACE_INT("Camera:Reprocess", 1);
        LOGH("scheduling framework reprocess");
        pthread_mutex_lock(&mReprocJobLock);
        // enqueu to post proc input queue
        m_inputFWKPPQ.enqueue((void *)frame);
        m_dataProcTh.sendCmd(CAMERA_CMD_TYPE_DO_NEXT_JOB, FALSE, FALSE);
        pthread_mutex_unlock(&mReprocJobLock);
    } else {
        jpeg_settings_t *jpeg_settings = (jpeg_settings_t *)m_jpegSettingsQ.dequeue();

        if (jpeg_settings == NULL) {
            LOGE("Cannot find jpeg settings");
            return BAD_VALUE;
        }

        LOGH("no need offline reprocess, sending to jpeg encoding");
        qcamera_hal3_jpeg_data_t *jpeg_job =
            (qcamera_hal3_jpeg_data_t *)malloc(sizeof(qcamera_hal3_jpeg_data_t));
        if (jpeg_job == NULL) {
            LOGE("No memory for jpeg job");
            return NO_MEMORY;
        }

        memset(jpeg_job, 0, sizeof(qcamera_hal3_jpeg_data_t));
        jpeg_job->fwk_frame = frame;
        jpeg_job->jpeg_settings = jpeg_settings;
        jpeg_job->metadata =
                (metadata_buffer_t *) frame->metadata_buffer.buffer;

        // enqueu to jpeg input queue
        m_inputJpegQ.enqueue((void *)jpeg_job);
        m_dataProcTh.sendCmd(CAMERA_CMD_TYPE_DO_NEXT_JOB, FALSE, FALSE);
    }

    return NO_ERROR;
}

/*===========================================================================
 * FUNCTION   : cancelFramePProc
 *
 * DESCRIPTION: cancel FramePost Processing request.
 *
 * PARAMETERS :
 *   @frame   : frame number for which postproc request need to be cancel.
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *
 *==========================================================================*/
int32_t QCamera3PostProcessor::cancelFramePProc(uint32_t framenum)
{
    if (isHdrCaptureFrame(framenum) || isDiscardedHdrFrame(framenum)) {
        cancelHdrCapture(framenum);
        return NO_ERROR;
    }
    return cancelQueuedFrame(framenum);
}

/*===========================================================================
 * FUNCTION   : processPPMetadata
 *
 * DESCRIPTION: enqueue data into dataProc thread
 *
 * PARAMETERS :
 *   @frame   : process metadata frame received from pic channel
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *
 *==========================================================================*/
int32_t QCamera3PostProcessor::processPPMetadata(mm_camera_super_buf_t *reproc_meta,
                               uint32_t framenum, bool dropFrame)
{
    LOGD("E");
    pthread_mutex_lock(&mReprocJobLock);

    qcamera_hal3_meta_pp_buffer_t *ppMetaBuf =
        (qcamera_hal3_meta_pp_buffer_t *)malloc(sizeof(qcamera_hal3_meta_pp_buffer_t));

    if (!ppMetaBuf) {
        pthread_mutex_unlock(&mReprocJobLock);
        return NO_MEMORY;
    }
    ppMetaBuf->metabuf = reproc_meta;
    ppMetaBuf->metaFrameNumber = framenum;
    ppMetaBuf->dropFrame = dropFrame;
    const bool cancelled = isQueuedFrameCancelled(framenum);
    bool queued = true;
    /* Need to send notifyError before meta for Error Buffer */
    if (cancelled) {
        queued = m_inputMetaQ.enqueue((void *)ppMetaBuf);
        m_dataProcTh.sendCmd(CAMERA_CMD_TYPE_CANCEL_PP_FRAME, FALSE, FALSE);
    } else if (!(m_inputPPQ.isEmpty())) {
        qcamera_hal3_pp_buffer_t *reproc_job = isFrameMatched(framenum);
        if(reproc_job != NULL) {
            ReprocessBuffer reproc;
            reproc.metaBuffer = ppMetaBuf;
            reproc.reprocBuf = reproc_job;
            mReprocessNode.push_back(reproc);
            LOGD("pp queue is not empty, do next job");
            m_dataProcTh.sendCmd(CAMERA_CMD_TYPE_DO_NEXT_JOB, FALSE, FALSE);
        } else {
             queued = m_inputMetaQ.enqueue((void *)ppMetaBuf);
        }
    } else {
             queued = m_inputMetaQ.enqueue((void *)ppMetaBuf);
       LOGD("pp queue is empty, not calling do next job");
    }
    pthread_mutex_unlock(&mReprocJobLock);
    if (!queued) {
        releaseMetadata(ppMetaBuf, this);
        free(ppMetaBuf);
        return -ECANCELED;
    }
    return NO_ERROR;
}


/*===========================================================================
 * FUNCTION   : isFrameMatched
 *
 * DESCRIPTION: corresponding meta detection
 *
 * PARAMETERS :
 *   @frame   : frame number
 *
 * RETURN     :
 *  TRUE if Frame is present
 *  FALSE if Frame is not released.
 *
 *==========================================================================*/
qcamera_hal3_pp_buffer_t* QCamera3PostProcessor::isFrameMatched(uint32_t resultFrameNumber)
{
    qcamera_hal3_pp_buffer_t *reprocess_job =
            (qcamera_hal3_pp_buffer_t *)
            m_inputPPQ.dequeue(matchReprocessFrameNum, (void*)&resultFrameNumber);
    if(reprocess_job != NULL)
        return reprocess_job;
    return NULL;
}

void QCamera3PostProcessor::eraseJpegSetting(uint32_t resultFrameNumber)
{
    jpeg_settings_t* jpeg_setting =
            (jpeg_settings_t *) m_jpegSettingsQ.dequeue(matchJpegSetting,(void*)&resultFrameNumber);
    if(jpeg_setting != NULL) {
        LOGH("Erasing Jpeg settings for frame number %d", resultFrameNumber);
        free(jpeg_setting);
        jpeg_setting = NULL;
    }
}

/*===========================================================================
 * FUNCTION   : processJpegSettingData
 *
 * DESCRIPTION: enqueue jpegSetting into dataProc thread
 *
 * PARAMETERS :
 *   @jpeg_settings : jpeg settings data received from pic channel
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *
 *==========================================================================*/
int32_t QCamera3PostProcessor::processJpegSettingData(
        jpeg_settings_t *jpeg_settings)
{
    if (!jpeg_settings) {
        LOGE("invalid jpeg settings pointer");
        return -EINVAL;
    }
    m_jpegSettingsQ.init();
    int32_t rc = m_jpegSettingsQ.enqueue((void *)jpeg_settings);
    return rc;
}

List<qcamera_hal3_mpo_data_t> QCamera3PostProcessor::mMpoInputData;

/*===========================================================================
 * FUNCTION   : processJpegData
 *
 * DESCRIPTION: handled Jpeg callback from encoder (BOKEH snapshot case).
 *              After receiving BOKEH, MAIN, DEPTH images composeMpo.
 *              handles releasing of snapshot buffer, reprocess frame.
 *
 * PARAMETERS :
 *   @status     : Jpeg error or success status.
 *   @client_hdl : __unused.
 *   @jobId      : Jpeg job id.
 *   @p_output   : Jpeg output image buffer.
 *   @userdata   : user handle for this callback.
 *
 * RETURN     : none
 *
 *==========================================================================*/
void QCamera3PostProcessor::processJpegData(jpeg_job_status_t status,
                                              uint32_t /*client_hdl*/,
                                              uint32_t jobId,
                                              mm_jpeg_output_t *p_output,
                                              void *userdata)
{
    QCamera3PostProcessor *obj = (QCamera3PostProcessor *)userdata;
    if(obj)
    {
        qcamera_hal3_jpeg_data_t *job = obj->findJpegJobByJobId(jobId);
        if((job == NULL) || (status == JPEG_JOB_STATUS_ERROR))
        {
            //Send Error buffer with status to MpoEvtHandle.
            LOGE("ERROR: JPEG Encoding failed");
        return;
        }

        qcamera_hal3_mpo_data_t mpo_input;

        mpo_input.jpeg_job = job;
        mpo_input.jpeg_image = *p_output;
        mpo_input.user_data = obj->mJpegUserData; //setting pic channel as userdata

        mMpoInputData.push_back(mpo_input);

        if(mMpoInputData.size() < MM_JPEG_MAX_MPO_IMAGES)
        {
            LOGH("need %d more jpeg images to compose mpo",
                                MM_JPEG_MAX_MPO_IMAGES - mMpoInputData.size());
            obj->doNextJob();
            return;
        } else{
            LOGI("Received %d images to compose MPO", mMpoInputData.size());
        }

        //getting the mpo output buffer from the main pic channel.
        QCamera3PicChannel *pic_channel = NULL;
        for(auto it = mMpoInputData.begin(); it != mMpoInputData.end(); it++)
        {
            qcamera_hal3_jpeg_data_t *job = it->jpeg_job;
            if(job->jpeg_settings->image_type == CAM_HAL3_JPEG_TYPE_MAIN)
            {
                pic_channel = (QCamera3PicChannel *)it->user_data;
                break;
            }
        }

        if(pic_channel == NULL)
        {
            LOGE("unexpected case: no pic channel to get mpo output buffer");
            return;
        }

        mm_jpeg_output_t mpo_output;
        memset (&mpo_output, 0, sizeof(mm_jpeg_output_t));
        pic_channel->getMpoOutputBuffer(&mpo_output);

        //main image should be bokeh output.
        //first aux will be main pic channel output.
        //second aux will be aux pic channel output.
        qcamera_hal3_mpo_compose_info_t mpo_info;
        memset (&mpo_info, 0, sizeof(qcamera_hal3_mpo_compose_info_t));
        for (auto it = mMpoInputData.begin(); it != mMpoInputData.end(); it++)
        {
            qcamera_hal3_jpeg_data_t *job = it->jpeg_job;
            if(job->jpeg_settings->image_type == CAM_HAL3_JPEG_TYPE_BOKEH)
            {
                LOGH("Received bokeh processed image");
                mpo_info.main_image = (it->jpeg_image);
            }
            else if(job->jpeg_settings->image_type == CAM_HAL3_JPEG_TYPE_MAIN) {
                LOGH("Received main image");
                mpo_info.aux_images[0] = (it->jpeg_image);
            }
            else if (job->jpeg_settings->image_type == CAM_HAL3_JPEG_TYPE_DEPTH) {
                LOGH("Received aux image");
                mpo_info.aux_images[1] = (it->jpeg_image);
            }
            cam_frame_len_offset_t offset;
            memset(&offset, 0, sizeof(cam_frame_len_offset_t));
            mm_camera_buf_def_t *jpeg_dump_buffer = NULL;
            //Dumping images.
            cam_dimension_t dim;
            dim.width = pic_channel->mCamera3Stream->width;
            dim.height = pic_channel->mCamera3Stream->height;
            jpeg_dump_buffer = (mm_camera_buf_def_t *)malloc(sizeof(mm_camera_buf_def_t));
            if(!jpeg_dump_buffer) {
                LOGE("Could not allocate jpeg dump buffer");
            } else {
                jpeg_dump_buffer->buffer = it->jpeg_image.buf_vaddr;
                jpeg_dump_buffer->frame_len = it->jpeg_image.buf_filled_len;
                jpeg_dump_buffer->frame_idx =
                    pic_channel->mJpegMemory.getFrameNumber(job->jpeg_settings->out_buf_index);
                pic_channel->dumpYUV(jpeg_dump_buffer, dim, offset, QCAMERA_DUMP_FRM_OUTPUT_JPEG);
                free(jpeg_dump_buffer);
            }
        }

        mpo_info.num_of_aux_image = MM_JPEG_MAX_MPO_IMAGES - 1;
        mpo_info.output = mpo_output;
        mpo_info.output.buf_filled_len = 0;

        //relaease snapshot buffer before sending mpo callback.
        for(auto it = mMpoInputData.begin(); it != mMpoInputData.end(); it++)
        {
            qcamera_hal3_jpeg_data_t *job = it->jpeg_job;
            QCamera3PicChannel * obj = (QCamera3PicChannel *)it->user_data;
            if(job->jpeg_settings->image_type == CAM_HAL3_JPEG_TYPE_MAIN
                 || job->jpeg_settings->image_type == CAM_HAL3_JPEG_TYPE_DEPTH)
            {
                obj->releaseSnapshotBuffer(job->src_reproc_frame);
            }
        }

        //compose MPO.
        //sends MPO event callback to pic channel;
        obj->composeMpo(mpo_info, pic_channel);

        //release JPEG JOB.
        auto it = mMpoInputData.begin();
        do {
            if(it == mMpoInputData.end())
            {
                break;
            }
            qcamera_hal3_jpeg_data_t *job = it->jpeg_job;
            QCamera3PicChannel * obj = (QCamera3PicChannel *)it->user_data;
            int bufIdx = job->jpeg_settings->out_buf_index;
             int frameNumber = obj->mJpegMemory.getFrameNumber(bufIdx);
            if((job->fwk_src_buffer != NULL) || (job->fwk_frame != NULL))
            {
                obj->releaseOfflineMemory(frameNumber);
            } else {
                //release offline buffers for MAIN and DEPTH (AUX channel).
                if(job->jpeg_settings->image_type != CAM_HAL3_JPEG_TYPE_BOKEH)
                {
                   obj->m_postprocessor.releaseOfflineBuffers(false);
                }
            }
            obj->freeBufferForJpeg(bufIdx);
            obj->m_postprocessor.releaseJpegJobData(job);
            free(job->jpeg_settings);
            free(job);
            mMpoInputData.erase(it);
            it = mMpoInputData.begin();
        }while(it != mMpoInputData.end());
    } else {
        LOGE("Error: Received NULL JPEG userdata");
    }
}

/*===========================================================================
 * FUNCTION   : processPPData
 *
 * DESCRIPTION: process received frame after reprocess.
 *
 * PARAMETERS :
 *   @frame   : received frame from reprocess channel.
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *
 * NOTE       : The frame after reprocess need to send to jpeg encoding.
 *==========================================================================*/
bool QCamera3PostProcessor::matchPPJob(void *data, void *, void *match_data)
{
    return static_cast<qcamera_hal3_pp_data_t *>(data)->frameNumber ==
            *static_cast<uint32_t *>(match_data);
}

int32_t QCamera3PostProcessor::beginHdrCapture(const uint32_t frameIds[3],
        uint32_t baseId, bool frontCamera)
{
    return beginBurstCapture(frameIds, 3, baseId, frontCamera, false);
}

int32_t QCamera3PostProcessor::beginLowLightCapture(const uint32_t frameIds[5],
        uint32_t baseId, bool frontCamera)
{
    return beginBurstCapture(frameIds, 5, baseId, frontCamera, true);
}

int32_t QCamera3PostProcessor::beginStillCapture(const uint32_t frameIds[4],
        uint32_t baseId, bool frontCamera, bool enhanceDetail)
{
    return beginBurstCapture(frameIds, 4, baseId, frontCamera, false, true, enhanceDetail);
}

int32_t QCamera3PostProcessor::beginDualCapture(uint32_t frameNumber, uint64_t token)
{
    if (!token || !m_parent->dualCapture()) return BAD_VALUE;
    return beginBurstCapture(&frameNumber, 1, frameNumber, false, false, false, false, token);
}

void QCamera3PostProcessor::notifyDualFrameReady()
{
    // Driver/metadata callbacks never enter another channel's worker or locks.
    m_dataProcTh.sendCmd(CAMERA_CMD_TYPE_DO_NEXT_JOB, FALSE, FALSE);
}

int32_t QCamera3PostProcessor::beginBurstCapture(const uint32_t *frameIds,
        unsigned count, uint32_t baseId, bool frontCamera, bool lowLight,
        bool still, bool enhanceDetail, uint64_t dualToken, int32_t nativeMode)
{
    if (!frameIds || frameIds[0] != baseId || (still && lowLight) ||
            (dualToken && (still || lowLight)) ||
            count != (dualToken ? 1U : still ? 4U : lowLight ? 5U : 3U))
            return BAD_VALUE;
    for (unsigned i = 0; i < count; ++i)
        for (unsigned j = 0; j < i; ++j)
            if (frameIds[i] == frameIds[j]) return BAD_VALUE;
    if (!(dualToken ? m_parent->dualCapture() != nullptr :
            still ? QCamera3StillAlgo::isAvailable() :
            lowLight ? QCamera3LowLightHdrAlgo::isAvailable() :
            QCamera3HdrAlgo::isAvailable())) return -ENOSYS;
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    // Full-resolution CPU merges on ARM can exceed the old five-second wait.
    // Match the per-burst JPEG watchdog while keeping a finite submission wait.
    deadline.tv_sec += 30;
    pthread_mutex_lock(&mHdrCaptureLock);
    const auto previous = std::atomic_load(&mHdrCaptureState);
    if (mHdrActive && previous && previous->dualToken && !mHdrBaseJob &&
            m_parent->dualPostprocessor() != this && m_parent->dualCapture() &&
            m_parent->dualCapture()->tokenFor(m_parent->getMyCamHandle(),
                    previous->frameIds[0]) != previous->dualToken) {
        // A silent auxiliary source can outlive the main JPEG fallback.
        // Retire its canceled collection before accepting the next capture;
        // late buffers remain tracked and are returned without a second JPEG.
        cancelHdrCapture(previous->frameIds[0]);
    }
    while (mHdrActive) {
        int rc = pthread_cond_timedwait(&mHdrCaptureCond, &mHdrCaptureLock, &deadline);
        if (rc != 0) {
            pthread_mutex_unlock(&mHdrCaptureLock);
            return -rc;
        }
    }
    memset(mHdrFrameIds, 0, sizeof(mHdrFrameIds));
    memcpy(mHdrFrameIds, frameIds, count * sizeof(*frameIds));
    memset(mHdrReceived, 0, sizeof(mHdrReceived));
    memset(mHdrExposure, 0, sizeof(mHdrExposure));
    memset(mHdrSensitivity, 0, sizeof(mHdrSensitivity));
    memset(mHdrDrcGain, 0, sizeof(mHdrDrcGain));
    memset(mHdrTimestamps, 0, sizeof(mHdrTimestamps));
    memset(mHdrImages, 0, sizeof(mHdrImages));
    mHdrBaseJob = NULL;
    mHdrBaseFrame = NULL;
    mHdrFrontCamera = frontCamera;
    const auto state = std::make_shared<HdrCaptureState>(frameIds, count, lowLight,
            still, enhanceDetail, dualToken, nativeMode);
    if (dualToken && m_parent->dualPostprocessor() != this) state->outputPending = false;
    for (unsigned i = 0; i < count; ++i) mHdrFrameStates[frameIds[i]] = state;
    std::atomic_store(&mHdrCaptureState, state);
    mHdrActive = true;
    pthread_mutex_unlock(&mHdrCaptureLock);
    return NO_ERROR;
}

bool QCamera3PostProcessor::isHdrCaptureFrame(uint32_t frameNumber)
{
    pthread_mutex_lock(&mHdrCaptureLock);
    const auto state = std::atomic_load(&mHdrCaptureState);
    bool found = false;
    for (unsigned i = 0; mHdrActive && i < state->count; ++i)
        found |= mHdrFrameIds[i] == frameNumber;
    pthread_mutex_unlock(&mHdrCaptureLock);
    return found;
}

bool QCamera3PostProcessor::isDiscardedHdrFrame(uint32_t frameNumber)
{
    pthread_mutex_lock(&mHdrCaptureLock);
    const auto tracked = mHdrFrameStates.find(frameNumber);
    bool found = tracked != mHdrFrameStates.end() &&
            tracked->second->stage != HdrCaptureState::COLLECTING;
    for (List<uint32_t>::iterator it = mHdrDiscardFrames.begin();
            !found && it != mHdrDiscardFrames.end(); ++it) found = *it == frameNumber;
    pthread_mutex_unlock(&mHdrCaptureLock);
    return found;
}

bool QCamera3PostProcessor::hasPendingHdrCapture(uint32_t frameNumber)
{
    pthread_mutex_lock(&mHdrCaptureLock);
    const auto current = std::atomic_load(&mHdrCaptureState);
    bool found = false;
    for (unsigned i = 0; mHdrActive && i < current->count; ++i)
        found |= frameNumber == mHdrFrameIds[i];
    const auto tracked = mHdrFrameStates.find(frameNumber);
    if (tracked != mHdrFrameStates.end()) {
        const auto &state = tracked->second;
        found |= state->outputPending && state->frameIds[0] == frameNumber;
    }
    for (List<uint32_t>::iterator it = mHdrFailedFrames.begin();
            !found && it != mHdrFailedFrames.end(); ++it) found = *it == frameNumber;
    pthread_mutex_unlock(&mHdrCaptureLock);
    return found;
}

void QCamera3PostProcessor::finishHdrGroupLocked()
{
    const auto state = std::atomic_load(&mHdrCaptureState);
    state->stage = state->cancelled.load() ? HdrCaptureState::FAILED :
            HdrCaptureState::READY;
    for (unsigned i = 0; i < state->count; ++i) {
        free(mHdrImages[i].data);
        mHdrImages[i].data = NULL;
        mHdrDiscardFrames.push_back(mHdrFrameIds[i]);
        if (state->cancelled.load()) mHdrFailedFrames.push_back(mHdrFrameIds[i]);
    }
    // Recent duplicates are bounded; actual outstanding owners are retained in
    // mHdrFrameStates until PP and framework output ownership are both gone.
    while (mHdrDiscardFrames.size() > 96)
        mHdrDiscardFrames.erase(mHdrDiscardFrames.begin());
    while (mHdrFailedFrames.size() > 96)
        mHdrFailedFrames.erase(mHdrFailedFrames.begin());
    mHdrBaseJob = NULL;
    mHdrBaseFrame = NULL;
    mHdrActive = false;
    pthread_cond_broadcast(&mHdrCaptureCond);
}

void QCamera3PostProcessor::abortHdrCapture()
{
    const auto state = std::atomic_load(&mHdrCaptureState);
    if (state) cancelHdrCapture(state->frameIds[0]);
}

bool QCamera3PostProcessor::cancelHdrCapture(uint32_t frameNumber)
{
    // Signal only this immutable group's token before waiting for a core call.
    // A newer begin cannot clear or inherit the old group's cancellation.
    auto state = std::atomic_load(&mHdrCaptureState);
    bool matches = false;
    for (unsigned i = 0; state && i < state->count; ++i)
        matches |= state->frameIds[i] == frameNumber;
    if (matches) {
        state->cancelled.store(true);
        if (state->dualToken && m_parent->dualCapture())
            m_parent->dualCapture()->cancel(state->dualToken);
    }
    pthread_mutex_lock(&mHdrCaptureLock);
    if (!matches) {
        const auto tracked = mHdrFrameStates.find(frameNumber);
        state = tracked != mHdrFrameStates.end() ? tracked->second : nullptr;
    }
    if (state && state == std::atomic_load(&mHdrCaptureState) && mHdrActive) {
        state->cancelled.store(true);
        discardHdrFrame(mHdrBaseJob, mHdrBaseFrame);
        finishHdrGroupLocked();
    }
    bool cancelled = state && state->stage == HdrCaptureState::FAILED;
    if (!state) {
        for (auto it = mHdrFailedFrames.begin(); it != mHdrFailedFrames.end(); ++it)
            cancelled |= *it == frameNumber;
    }
    // A JPEG already handed to the encoder retains its output buffer. This
    // return value is the boundary used by PicChannel's timeout error return.
    pthread_mutex_unlock(&mHdrCaptureLock);
    if (cancelled && state) {
        // The cancellation queue has its own lock. Inputs still waiting for
        // metadata also need to release their source slots; a CPP callback
        // cannot do that for work which was never submitted.
        for (unsigned i = 0; i < state->count; ++i)
            cancelQueuedFrame(state->frameIds[i]);
    }
    return cancelled;
}

void QCamera3PostProcessor::retireHdrFrame(uint32_t frameNumber)
{
    pthread_mutex_lock(&mHdrCaptureLock);
    const auto tracked = mHdrFrameStates.find(frameNumber);
    if (tracked != mHdrFrameStates.end()) {
        const auto state = tracked->second;
        for (unsigned i = 0; i < state->count; ++i) {
            if (state->frameIds[i] == frameNumber) {
                state->pending[i] = false;
                if (i != 0 || !state->outputPending) mHdrFrameStates.erase(tracked);
                break;
            }
        }
    }
    pthread_mutex_unlock(&mHdrCaptureLock);
}

void QCamera3PostProcessor::completeHdrOutput(uint32_t frameNumber)
{
    pthread_mutex_lock(&mHdrCaptureLock);
    const auto tracked = mHdrFrameStates.find(frameNumber);
    if (tracked != mHdrFrameStates.end() && tracked->second->frameIds[0] == frameNumber) {
        const auto state = tracked->second;
        state->outputPending = false;
        if (!state->pending[0]) mHdrFrameStates.erase(tracked);
    }
    for (auto it = mHdrFailedFrames.begin(); it != mHdrFailedFrames.end();) {
        if (*it == frameNumber) it = mHdrFailedFrames.erase(it);
        else ++it;
    }
    pthread_mutex_unlock(&mHdrCaptureLock);
}

void QCamera3PostProcessor::discardHdrFrame(qcamera_hal3_pp_data_t *job,
        mm_camera_super_buf_t *frame)
{
    if (frame && job) releaseOfflineBuffersForFrame(job->frameNumber);
    if (frame) {
        for (int8_t i = 0; i < m_ppChannelCnt; ++i) {
            if (m_pReprocChannel[i] &&
                    m_pReprocChannel[i]->getMyHandle() == frame->ch_id) {
                m_pReprocChannel[i]->bufDone(frame);
                break;
            }
        }
        if (!job || frame != job->src_frame) free(frame);
    }
    if (job) {
        retireHdrFrame(job->frameNumber);
        if (job->src_frame) m_parent->freeBufferForFrame(job->src_frame);
        if (job->reprocessed_src_frame && job->reprocessed_src_frame != frame) {
            for (int8_t i = 0; i < m_ppChannelCnt; ++i) {
                if (m_pReprocChannel[i] && m_pReprocChannel[i]->getMyHandle() ==
                        job->reprocessed_src_frame->ch_id) {
                    m_pReprocChannel[i]->bufDone(job->reprocessed_src_frame);
                    break;
                }
            }
            free(job->reprocessed_src_frame);
        }
        free(job->jpeg_settings);
        if (job->ppOutput_jpeg_settings != job->jpeg_settings)
            free(job->ppOutput_jpeg_settings);
        releasePPJobData(job);
        free(job);
    }
}

bool QCamera3PostProcessor::getHdrImage(mm_camera_super_buf_t *frame,
        QCamera3HdrAlgo::Image &image, QCamera3StreamMem *&memory)
{
    memset(&image, 0, sizeof(image));
    memory = NULL;
    if (!frame || frame->num_bufs != 1 || !frame->bufs[0]) return false;
    mm_camera_buf_def_t *buf = frame->bufs[0];
    QCamera3Stream *stream = NULL;
    for (int8_t i = 0; i < m_ppChannelCnt; ++i) {
        if (m_pReprocChannel[i] && m_pReprocChannel[i]->getMyHandle() == frame->ch_id) {
            stream = m_pReprocChannel[i]->getStreamByHandle(buf->stream_id);
            break;
        }
    }
    if (!stream && m_parent) stream = m_parent->getStreamByHandle(buf->stream_id);
    if (!stream) return false;
    cam_dimension_t dim = {};
    cam_frame_len_offset_t layout = {};
    cam_format_t format;
    if (stream->getFrameDimension(dim) != NO_ERROR ||
            stream->getFrameOffset(layout) != NO_ERROR ||
            stream->getFormat(format) != NO_ERROR || format != CAM_FORMAT_YUV_420_NV21 ||
            layout.num_planes != 2 || dim.width <= 0 || dim.height <= 0) return false;
    memory = stream->getStreamBufs();
    if (!memory) return false;
    const ssize_t size = memory->getSize(buf->buf_idx);
    if (size <= 0 || !buf->buffer || layout.frame_len > buf->frame_len ||
            buf->frame_len > uint64_t(size) ||
            uint64_t(layout.mp[0].len) + layout.mp[1].len > layout.frame_len ||
            layout.mp[0].offset >= layout.mp[0].len ||
            layout.mp[1].offset >= layout.mp[1].len ||
            layout.mp[0].scanline < dim.height ||
            layout.mp[1].scanline < dim.height / 2) return false;
    image.data = static_cast<uint8_t *>(buf->buffer);
    image.length = size_t(size);
    image.width = dim.width;
    image.height = dim.height;
    image.yOffset = layout.mp[0].offset;
    image.uvOffset = size_t(layout.mp[0].len) + layout.mp[1].offset;
    image.yStride = layout.mp[0].stride_in_bytes > 0 ?
            layout.mp[0].stride_in_bytes : layout.mp[0].stride;
    image.uvStride = layout.mp[1].stride_in_bytes > 0 ?
            layout.mp[1].stride_in_bytes : layout.mp[1].stride;
    image.yScanline = layout.mp[0].scanline;
    image.uvScanline = layout.mp[1].scanline;
    if (!QCamera3HdrAlgo::validImage(image)) return false;
    // In addition to the allocation bound, each visible plane must fit its own
    // length. A bad Y extent cannot borrow space belonging to the UV plane.
    if (uint64_t(image.yOffset) + uint64_t(image.yStride) * (image.height - 1) +
            image.width > layout.mp[0].len ||
            uint64_t(layout.mp[1].offset) + uint64_t(image.uvStride) *
            (image.height / 2 - 1) + image.width > layout.mp[1].len) return false;
    return memory->invalidateCache(buf->buf_idx) == NO_ERROR;
}

bool QCamera3PostProcessor::collectHdrFrame(qcamera_hal3_pp_data_t *&job,
        mm_camera_super_buf_t *&frame)
{
    pthread_mutex_lock(&mHdrCaptureLock);
    const auto state = std::atomic_load(&mHdrCaptureState);
    int slot = -1;
    for (unsigned i = 0; mHdrActive && i < state->count; ++i)
        if (job->frameNumber == mHdrFrameIds[i]) slot = i;
    if (slot < 0) {
        bool discard = mHdrFrameStates.count(job->frameNumber) != 0;
        for (List<uint32_t>::iterator it = mHdrDiscardFrames.begin();
                it != mHdrDiscardFrames.end(); ++it) discard |= *it == job->frameNumber;
        if (discard) discardHdrFrame(job, frame);
        pthread_mutex_unlock(&mHdrCaptureLock);
        return discard;
    }
    if (mHdrReceived[slot]) {
        discardHdrFrame(job, frame);
        pthread_mutex_unlock(&mHdrCaptureLock);
        return true;
    }
    mHdrReceived[slot] = true;
    QCamera3HdrAlgo::Image source;
    QCamera3StreamMem *memory;
    if (getHdrImage(frame, source, memory)) {
        const uint64_t ySize = uint64_t(source.width) * source.height;
        const uint64_t size = ySize + ySize / 2;
        if (size <= SIZE_MAX) {
            QCamera3HdrAlgo::Image &copy = mHdrImages[slot];
            copy = source;
            copy.data = static_cast<uint8_t *>(malloc(size_t(size)));
            copy.length = size_t(size);
            copy.yOffset = 0;
            copy.uvOffset = size_t(ySize);
            copy.yStride = copy.uvStride = source.width;
            copy.yScanline = source.height;
            copy.uvScanline = source.height / 2;
            if (copy.data) {
                for (uint32_t row = 0; row < source.height; ++row)
                    memcpy(copy.data + size_t(row) * copy.yStride,
                            source.data + source.yOffset + size_t(row) * source.yStride,
                            source.width);
                for (uint32_t row = 0; row < source.height / 2; ++row)
                    memcpy(copy.data + copy.uvOffset + size_t(row) * copy.uvStride,
                            source.data + source.uvOffset + size_t(row) * source.uvStride,
                            source.width);
            }
        }
    }
    if (job->metadata) {
        int64_t exposure = 0;
        int32_t sensitivity = 0;
        IF_META_AVAILABLE(int64_t, exposureValue, CAM_INTF_META_SENSOR_EXPOSURE_TIME, job->metadata) {
            exposure = *exposureValue;
        }
        IF_META_AVAILABLE(int32_t, sensitivityValue, CAM_INTF_META_SENSOR_SENSITIVITY, job->metadata) {
            sensitivity = *sensitivityValue;
        }
        if (exposure > 0 && sensitivity > 0)
            mHdrExposure[slot] = double(exposure) * sensitivity;
        mHdrSensitivity[slot] = sensitivity;
        IF_META_AVAILABLE(int64_t, frameTimestamp, CAM_INTF_META_SENSOR_TIMESTAMP,
                job->metadata) {
            if (*frameTimestamp > 0) mHdrTimestamps[slot] = *frameTimestamp;
        }
        if (job->metadata->oppo_algorithm.llhdr_drc_valid)
            mHdrDrcGain[slot] = job->metadata->oppo_algorithm.llhdr_drc_gain;
    }
    if (slot == 0) {
        mHdrBaseJob = job;
        mHdrBaseFrame = frame;
    } else {
        // CPP may have fewer output slots than the burst's image count. Return each internal
        // output immediately after copying, allowing the next exposure to run.
        discardHdrFrame(job, frame);
    }
    if (state->dualToken) {
        QCamera3DualCapture *capture = m_parent->dualCapture();
        QCamera3PostProcessor *owner = m_parent->dualPostprocessor();
        const int32_t rc = capture && QCamera3HdrAlgo::validImage(mHdrImages[0]) ?
                capture->submitImage(state->dualToken, m_parent->getMyCamHandle(),
                        state->frameIds[0], mHdrImages[0]) : -EINVAL;
        if (rc != NO_ERROR && capture) capture->cancel(state->dualToken);
        if (owner != this) {
            // The shared collector has copied the tele input. No auxiliary
            // framework output or JPEG ownership crosses into the main channel.
            discardHdrFrame(mHdrBaseJob, mHdrBaseFrame);
            finishHdrGroupLocked();
        }
        if (owner) owner->notifyDualFrameReady();
        pthread_mutex_unlock(&mHdrCaptureLock);
        return true;
    }
    for (unsigned i = 0; i < state->count; ++i)
        if (!mHdrReceived[i]) {
            pthread_mutex_unlock(&mHdrCaptureLock);
            return true;
        }
    job = mHdrBaseJob;
    frame = mHdrBaseFrame;
    int32_t rc = -EINVAL;
    const bool superPhoto = state->still && job->jpeg_settings &&
            job->jpeg_settings->native_still_mode == OPPO_NATIVE_SUPERPHOTO;
    const char *algorithm = superPhoto ? "VDSuperPhoto" :
            state->still ? "MFNR" : state->lowLight ? "LLHDR" : "HDR";
    QCamera3HdrAlgo::Image destination;
    bool settled = true;
    for (unsigned i = 0; i < state->count; ++i) settled &= mHdrExposure[i] > 0;
    if (state->still) {
        for (unsigned i = 1; i < state->count; ++i)
            settled &= mHdrExposure[i] >= mHdrExposure[0] * 0.90 &&
                    mHdrExposure[i] <= mHdrExposure[0] * 1.10;
    } else if (state->lowLight) {
        for (unsigned i = 1; i < 4; ++i)
            settled &= mHdrExposure[i] >= mHdrExposure[0] * 0.85 &&
                    mHdrExposure[i] <= mHdrExposure[0] * 1.15;
        settled &= mHdrExposure[4] >= mHdrExposure[0] * 0.125 &&
                mHdrExposure[4] <= mHdrExposure[0] * 0.5;
    } else {
        settled &= mHdrExposure[1] < mHdrExposure[0] * 0.95 &&
                mHdrExposure[2] > mHdrExposure[0] * 1.05;
    }
    if (!settled) {
        LOGE("HAL3 burst exposure bracket did not settle; returning base JPEG");
    } else if (getHdrImage(frame, destination, memory)) {
        if (state->still) {
            if (superPhoto) {
                QCamera3HardwareInterface *hardware =
                        static_cast<QCamera3HardwareInterface *>(m_parent->mUserData);
                const char *sensor = hardware ? hardware->getOppoSensorName() : nullptr;
                rc = sensor ? QCamera3StockSuperPhoto::process(mHdrImages,
                        state->count, destination, sensor,
                        mHdrSensitivity[0], mHdrFrontCamera, &state->cancelled) : -EINVAL;
            } else {
                QCamera3StillAlgo::Options options;
                options.referenceIndex = 0; // Framework metadata belongs to base.
                options.enhanceDetails = state->enhanceDetail;
                QCamera3StillAlgo::Result result;
                rc = QCamera3StillAlgo::process(mHdrImages, state->count, destination,
                        options, &result, &state->cancelled);
                if (rc == NO_ERROR)
                    LOGI("HAL3 still fusion accepted %u/%u frames", result.acceptedFrames, state->count);
            }
        } else {
            rc = state->lowLight ? QCamera3LowLightHdrAlgo::process(mHdrImages,
                destination, mHdrSensitivity[0], mHdrDrcGain[0],
                mHdrFrontCamera ? 1 : 0, &state->cancelled) :
                QCamera3HdrAlgo::process(mHdrImages, destination, mHdrFrontCamera,
                    &state->cancelled);
        }
        if (rc == NO_ERROR && state->nativeMode == 0 && !state->lowLight && !state->still &&
                job->metadata) {
            bool denoise = false, sharpen = false;
            IF_META_AVAILABLE(uint32_t, mode, CAM_INTF_META_NOISE_REDUCTION_MODE, job->metadata) {
                denoise = *mode == ANDROID_NOISE_REDUCTION_MODE_HIGH_QUALITY;
            }
            IF_META_AVAILABLE(cam_edge_application_t, edge, CAM_INTF_META_EDGE_MODE, job->metadata) {
                sharpen = edge->edge_mode != ANDROID_EDGE_MODE_OFF;
            }
            if (denoise) {
                const int32_t asd = QCamera3HdrDenoise::process(destination,
                        mHdrSensitivity[0], sharpen, &state->cancelled);
                if (asd != NO_ERROR)
                    LOGW("HAL3 NCF/ASD stage unavailable (%d); keeping merged HDR", asd);
            }
        }
        if (rc == NO_ERROR) {
            rc = memory->cleanCache(frame->bufs[0]->buf_idx);
            if (rc != NO_ERROR) {
                const QCamera3HdrAlgo::Image &base = mHdrImages[0];
                for (uint32_t row = 0; row < base.height; ++row)
                    memcpy(destination.data + destination.yOffset + size_t(row) * destination.yStride,
                            base.data + size_t(row) * base.yStride, base.width);
                for (uint32_t row = 0; row < base.height / 2; ++row)
                    memcpy(destination.data + destination.uvOffset + size_t(row) * destination.uvStride,
                            base.data + base.uvOffset + size_t(row) * base.uvStride, base.width);
                if (memory->cleanCache(frame->bufs[0]->buf_idx) != NO_ERROR) {
                    LOGE("HAL3 burst base fallback cache synchronization failed");
                    state->cancelled.store(true);
                }
            }
        }
    }
    if (rc != NO_ERROR) {
        LOGE("HAL3 %s fusion failed (%d); returning unfused base JPEG",
                algorithm, rc);
    } else {
        LOGI("HAL3 %s fused frame %u using %u inputs",
                algorithm, job->frameNumber, state->count);
    }
    nativeAlgorithmTrace(job->jpeg_settings, algorithm, rc, job->frameNumber, state->count,
            static_cast<QCamera3HardwareInterface *>(m_parent->mUserData));
    const bool discard = state->cancelled.load();
    if (discard) discardHdrFrame(job, frame);
    finishHdrGroupLocked();
    pthread_mutex_unlock(&mHdrCaptureLock);
    return discard;
}

bool QCamera3PostProcessor::fallbackDualCapture(uint32_t frameNumber)
{
    // The output watchdog may expire while the auxiliary camera is silent.
    // Transfer the retained main frame to JPEG before timeout reclaims its
    // framework buffer. The recursive group lock serializes flush and worker.
    pthread_mutex_lock(&mHdrCaptureLock);
    const auto state = std::atomic_load(&mHdrCaptureState);
    QCamera3DualCapture *capture = m_parent->dualCapture();
    bool handedOff = false;
    if (mHdrActive && state && state->dualToken && state->frameIds[0] == frameNumber &&
            !state->cancelled.load() && mHdrBaseJob && capture &&
            m_parent->dualPostprocessor() == this) {
        capture->cancel(state->dualToken);
        processDualCapture();
        handedOff = state->stage == HdrCaptureState::HANDED_OFF;
    }
    pthread_mutex_unlock(&mHdrCaptureLock);
    return handedOff;
}

void QCamera3PostProcessor::processDualCapture()
{
    pthread_mutex_lock(&mHdrCaptureLock);
    const auto state = std::atomic_load(&mHdrCaptureState);
    QCamera3DualCapture *capture = m_parent->dualCapture();
    if (!mHdrActive || !state || !state->dualToken || !mHdrBaseJob ||
            m_parent->dualPostprocessor() != this || !capture) {
        pthread_mutex_unlock(&mHdrCaptureLock);
        return;
    }
    const bool alive = capture->tokenFor(m_parent->getMyCamHandle(), state->frameIds[0]) ==
            state->dualToken;
    if (alive && !capture->isReady(state->dualToken) && !state->cancelled.load()) {
        pthread_mutex_unlock(&mHdrCaptureLock);
        return;
    }
    qcamera_hal3_pp_data_t *job = mHdrBaseJob;
    mm_camera_super_buf_t *frame = mHdrBaseFrame;
    QCamera3HdrAlgo::Image destination;
    QCamera3StreamMem *memory = nullptr;
    const bool destinationValid = getHdrImage(frame, destination, memory);
    int32_t rc = alive && destinationValid ?
            capture->process(state->dualToken, destination) : -ECANCELED;
    if (rc == NO_ERROR) rc = memory->cleanCache(frame->bufs[0]->buf_idx);
    bool fallback = false;
    if (rc != NO_ERROR && !state->cancelled.load() && destinationValid &&
            QCamera3HdrAlgo::validImage(mHdrImages[0])) {
        // The collector/core commits only a successful result. Restore the
        // owned main image as well when cache synchronization failed after a
        // successful merge. A failed pair must still produce one base JPEG.
        const auto &base = mHdrImages[0];
        if (base.width == destination.width && base.height == destination.height) {
            for (uint32_t row = 0; row < base.height; ++row)
                memcpy(destination.data + destination.yOffset + size_t(row) * destination.yStride,
                        base.data + base.yOffset + size_t(row) * base.yStride, base.width);
            for (uint32_t row = 0; row < base.height / 2; ++row)
                memcpy(destination.data + destination.uvOffset + size_t(row) * destination.uvStride,
                        base.data + base.uvOffset + size_t(row) * base.uvStride, base.width);
            fallback = memory->cleanCache(frame->bufs[0]->buf_idx) == NO_ERROR;
        }
    }
    capture->cancel(state->dualToken);
    if (state->cancelled.load() || (rc != NO_ERROR && !fallback)) {
        // Flush/cancellation must never deliver a late successful output.
        state->cancelled.store(true);
        discardHdrFrame(job, frame);
        LOGE("HAL3 dual capture has no deliverable main frame %u: %d", state->frameIds[0], rc);
        finishHdrGroupLocked();
    } else {
        LOGI("HAL3 dual capture frame %u: status %d, main fallback %d",
                job->frameNumber, rc, fallback);
        // The mode-8 disparity-relight route was deleted with the 2026-10-17
        // B2-b/B2-c batch, so the paired collector is always the bokeh core.
        const char *algorithm = "ArcSoftDualBokeh";
        nativeAlgorithmTrace(job->jpeg_settings, algorithm, rc, job->frameNumber, 2,
                static_cast<QCamera3HardwareInterface *>(m_parent->mUserData));
        finishHdrGroupLocked();
        queueJpegFrame(job, frame, nullptr);
    }
    pthread_mutex_unlock(&mHdrCaptureLock);
}

int32_t QCamera3PostProcessor::processPPData(mm_camera_super_buf_t *frame,
        const metadata_buffer_t *p_metadata, uint32_t frameNumber)
{
    qcamera_hal3_pp_data_t *job = static_cast<qcamera_hal3_pp_data_t *>(
            frameNumber == UINT32_MAX ? m_ongoingPPQ.dequeue() :
            m_ongoingPPQ.dequeue(matchPPJob, &frameNumber));
    qcamera_hal3_pp_data_t *pending_job;
    ATRACE_INT("Camera:Reprocess", 0);
    if (job == NULL || ((NULL == job->src_frame) && (NULL == job->fwk_src_frame))) {
        LOGE("Cannot find reprocess job");
        // Every orphan output still owns a CPP slot, regardless of how old its
        // frame number is. Return it even after recent duplicate records expire.
        discardHdrFrame(job, frame);
        return BAD_VALUE;
    }
    const bool hal3Hdr = isHdrCaptureFrame(job->frameNumber) ||
            isDiscardedHdrFrame(job->frameNumber);
    // Keep abort/flush from reclaiming the base between collection and JPEG
    // queue ownership transfer. Helpers also take this recursive group lock.
    struct HdrHandoffLock {
        pthread_mutex_t *mutex;
        explicit HdrHandoffLock(pthread_mutex_t *m) : mutex(m) {
            if (mutex) pthread_mutex_lock(mutex);
        }
        ~HdrHandoffLock() { if (mutex) pthread_mutex_unlock(mutex); }
    } hdrHandoff(hal3Hdr ? &mHdrCaptureLock : NULL);
    std::shared_ptr<HdrCaptureState> hdrState;
    if (hal3Hdr) {
        const auto tracked = mHdrFrameStates.find(job->frameNumber);
        if (tracked != mHdrFrameStates.end()) hdrState = tracked->second;
    }
    if (hal3Hdr && (job->pp_ch_idx + 1) >= m_ppChannelCnt) {
        // The original matched sensor result owns exposure, ISO and DRC.
        // Offline CPP metadata may be partial; it cannot replace those inputs.
        if (collectHdrFrame(job, frame)) return NO_ERROR;
        // The final arriving frame need not be the base exposure. Its metadata
        // must not overwrite the base frame's metadata before JPEG encoding.
        p_metadata = NULL;
    }
    LOGD("jpeg settings is :%p and %d",job->jpeg_settings, m_ongoingPPQ.getCurrentSize());
    if (job->jpeg_settings == NULL && !m_parent->m_bQuadraChannel &&
            !(hal3Hdr && (job->pp_ch_idx + 1) < m_ppChannelCnt)) {
        LOGE("Cannot find jpeg settings");
        if (hal3Hdr) {
            cancelHdrCapture(job->frameNumber);
            if (hdrState) hdrState->stage = HdrCaptureState::FAILED;
            discardHdrFrame(job, frame);
        }
        return BAD_VALUE;
    }

    bool hdr_snapshot = FALSE;
    if((job->jpeg_settings != NULL && job->jpeg_settings->hdr_snapshot == 1)) {
        hdr_snapshot = TRUE;
    }

    bool multiframe_snapshot = FALSE;
    if((job->jpeg_settings != NULL && job->jpeg_settings->multiframe_snapshot == 1)) {
        multiframe_snapshot = TRUE;
    }

    while((hdr_snapshot == TRUE || multiframe_snapshot == TRUE) && (!m_ongoingPPQ.isEmpty()) ) {
        LOGD(" Checking if empty");
        pending_job = (qcamera_hal3_pp_data_t *)m_ongoingPPQ.dequeue();
        if ((pending_job != NULL)) {
            LOGD("free reprocessed buffer");
            m_parent->freeBufferForFrame(pending_job->src_frame);
            m_parent->metadataBufDone(pending_job->src_metadata);
        }
    }

    if (job->jpeg_settings != NULL && job->jpeg_settings->zsl_snapshot) {
        m_parent->freeBufferForFrame(job->src_frame);
        job->src_frame = NULL;
    }

    if ((job->pp_ch_idx+1) < m_ppChannelCnt) {
        job->pp_ch_idx++;
        LOGH("next pp index:%d.", job->pp_ch_idx);

        if (job->fwk_src_frame != NULL) {
            LOGD("reprocess for fwk input frame.");
            if (p_metadata != NULL) {
                memcpy(job->fwk_src_frame->metadata_buffer.buffer, p_metadata,
                            sizeof(metadata_buffer_t));
            }
        } else if (job->src_frame != NULL) {
            LOGD("reprocess for non-fwk input frame.");
            if (!hal3Hdr && p_metadata != NULL && job->metadata != NULL) {
                memcpy(job->metadata, p_metadata, sizeof(metadata_buffer_t));
            }
        }

        // the src frame here should be from output of previous reprocess channel
        job->reprocessed_src_frame = frame;
        if (!m_inputMultiReprocQ.enqueue(job)) {
            if (hdrState) {
                cancelHdrCapture(job->frameNumber);
                hdrState->stage = HdrCaptureState::FAILED;
            }
            releaseOfflineBuffersForFrame(job->frameNumber);
            releaseOngoingPPData(job, this);
            free(job);
            return DEAD_OBJECT;
        }

        m_dataProcTh.sendCmd(CAMERA_CMD_TYPE_DO_NEXT_JOB, FALSE, FALSE);
        return NO_ERROR;
    }

    return queueJpegFrame(job, frame, p_metadata);
}

int32_t QCamera3PostProcessor::queueJpegFrame(qcamera_hal3_pp_data_t *job,
        mm_camera_super_buf_t *frame, const metadata_buffer_t *p_metadata)
{
    struct HandoffLock {
        pthread_mutex_t &mutex;
        explicit HandoffLock(pthread_mutex_t &m) : mutex(m) { pthread_mutex_lock(&mutex); }
        ~HandoffLock() { pthread_mutex_unlock(&mutex); }
    } handoff(mHdrCaptureLock);
    const auto tracked = mHdrFrameStates.find(job->frameNumber);
    const auto hdrState = tracked == mHdrFrameStates.end() ? nullptr : tracked->second;
    const bool hal3Hdr = hdrState != nullptr;
    qcamera_hal3_jpeg_data_t *jpeg_job =
        (qcamera_hal3_jpeg_data_t *)malloc(sizeof(qcamera_hal3_jpeg_data_t));
    if (jpeg_job == NULL) {
        LOGE("No memory for jpeg job");
        if (hal3Hdr) {
            cancelHdrCapture(job->frameNumber);
            if (hdrState) hdrState->stage = HdrCaptureState::FAILED;
            discardHdrFrame(job, frame);
        }
        return NO_MEMORY;
    }

    if (hdrState && hdrState->cancelled.load()) {
        hdrState->stage = HdrCaptureState::FAILED;
        discardHdrFrame(job, frame);
        free(jpeg_job);
        return NO_ERROR;
    }

    memset(jpeg_job, 0, sizeof(qcamera_hal3_jpeg_data_t));
    jpeg_job->src_frame = frame;
    if(frame != job->src_frame)
        jpeg_job->src_reproc_frame = job->src_frame;
    if (NULL == job->fwk_src_frame) {
        jpeg_job->metadata = job->metadata;
    } else {
        jpeg_job->metadata =
                (metadata_buffer_t *) job->fwk_src_frame->metadata_buffer.buffer;
        jpeg_job->fwk_src_buffer = job->fwk_src_frame;
    }
    if (p_metadata != NULL) {
        // update metadata content with input buffer
        memcpy(jpeg_job->metadata, p_metadata, sizeof(metadata_buffer_t));
    }
    jpeg_job->src_metadata = job->src_metadata;
    jpeg_job->jpeg_settings = job->jpeg_settings;

    if (job->reprocessed_src_frame != NULL) {
        LOGD("release output buffers of previous reprocess channel");
        for (int8_t i = 0; i < m_ppChannelCnt; i++) {
            if (m_pReprocChannel[i] != NULL &&
                m_pReprocChannel[i]->getMyHandle() == job->reprocessed_src_frame->ch_id) {
                int32_t rc = m_pReprocChannel[i]->bufDone(job->reprocessed_src_frame);
                if (NO_ERROR != rc) {
                    LOGE("bufDone error: %d", rc);
                }
                break;
            }
        }
        free(job->reprocessed_src_frame);
        job->reprocessed_src_frame = NULL;
    }

    if (hdrState) {
        // Cancel may signal after the last check, but cannot reclaim an output
        // across this locked ownership transfer. Timeout then defers to JPEG.
        hdrState->stage = HdrCaptureState::HANDED_OFF;
        retireHdrFrame(job->frameNumber);
    }
    // free pp job buf
    free(job);

    // enqueue transfers ownership only if the queue is still active. A CPP
    // callback racing STOP must release its payload instead of leaking it.
    if (!m_inputJpegQ.enqueue((void *)jpeg_job)) {
        if (hdrState) {
            hdrState->cancelled.store(true);
            hdrState->stage = HdrCaptureState::FAILED;
        }
        if (jpeg_job->jpeg_settings)
            releaseOfflineBuffersForFrame(jpeg_job->jpeg_settings->frame_number);
        if (jpeg_job->src_reproc_frame)
            m_parent->freeBufferForFrame(jpeg_job->src_reproc_frame);
        releaseJpegJobData(jpeg_job);
        free(jpeg_job);
        return DEAD_OBJECT;
    }

    // wait up data proc thread
    m_dataProcTh.sendCmd(CAMERA_CMD_TYPE_DO_NEXT_JOB, FALSE, FALSE);

    return NO_ERROR;
}

/*===========================================================================
 * FUNCTION   : dequeuePPJob
 *
 * DESCRIPTION: find a postprocessing job from ongoing pp queue by frame number
 *
 * PARAMETERS :
 *   @frameNumber : frame number for the pp job
 *
 * RETURN     : ptr to a pp job struct. NULL if not found.
 *==========================================================================*/
qcamera_hal3_pp_data_t *QCamera3PostProcessor::dequeuePPJob(uint32_t frameNumber) {
    qcamera_hal3_pp_data_t *pp_job = NULL;
    pp_job = (qcamera_hal3_pp_data_t *)m_ongoingPPQ.dequeue();

    if (pp_job == NULL) {
        LOGE("Fatal: ongoing PP queue is empty");
        return NULL;
    }
    if (pp_job->fwk_src_frame &&
            (pp_job->fwk_src_frame->frameNumber != frameNumber)) {
        LOGE("head of pp queue doesn't match requested frame number");
    }
    return pp_job;
}

/*===========================================================================
 * FUNCTION   : findJpegJobByJobId
 *
 * DESCRIPTION: find a jpeg job from ongoing Jpeg queue by its job ID
 *
 * PARAMETERS :
 *   @jobId   : job Id of the job
 *
 * RETURN     : ptr to a jpeg job struct. NULL if not found.
 *
 * NOTE       : Currently only one job is sending to mm-jpeg-interface for jpeg
 *              encoding. Therefore simply dequeue from the ongoing Jpeg Queue
 *              will serve the purpose to find the jpeg job.
 *==========================================================================*/
qcamera_hal3_jpeg_data_t *QCamera3PostProcessor::findJpegJobByJobId(uint32_t jobId)
{
    qcamera_hal3_jpeg_data_t * job = NULL;
    if (jobId == 0) {
        LOGE("not a valid jpeg jobId");
        return NULL;
    }

    QCamera3HardwareInterface* hal_obj = (QCamera3HardwareInterface*)m_parent->mUserData;
    if(!hal_obj->isDualCamera())
    {
        mFreeJpegSessions.push_back(mJpegSessionId);
        mJpegSessionId = 0;
    }
    // currely only one jpeg job ongoing, so simply dequeue the head
    job = (qcamera_hal3_jpeg_data_t *)m_ongoingJpegQ.dequeue();
    return job;
}

int QCamera3PostProcessor::releaseFreeJpegSessions()
{
    int ret = NO_ERROR;
    auto it = mFreeJpegSessions.begin();
    while(it != mFreeJpegSessions.end())
    {
        ret = mJpegHandle.destroy_session(*it);
        if(ret != NO_ERROR)
        {
            LOGE("failed to destroy jpeg session %d, non-fatal", *it);
        }
        it = mFreeJpegSessions.erase(it);
    }
    return ret;
}

/*===========================================================================
 * FUNCTION   : releasePPInputData
 *
 * DESCRIPTION: callback function to release post process input data node
 *
 * PARAMETERS :
 *   @data      : ptr to post process input data
 *   @user_data : user data ptr (QCamera3Reprocessor)
 *
 * RETURN     : None
 *==========================================================================*/
void QCamera3PostProcessor::releasePPInputData(void *data, void *user_data)
{
    QCamera3PostProcessor *pme = (QCamera3PostProcessor *)user_data;
    if (NULL != pme) {
        qcamera_hal3_pp_buffer_t *buf = (qcamera_hal3_pp_buffer_t *)data;
        if (NULL != buf) {
            if (buf->input) {
                if (buf->input->num_bufs == 1 && buf->input->bufs[0] &&
                        buf->input->bufs[0]->stream_type == CAM_STREAM_TYPE_SNAPSHOT)
                    pme->m_parent->freeBufferForFrame(buf->input);
                else
                    pme->releaseSuperBuf(buf->input);
                free(buf->input);
                buf->input = NULL;
            }
        }
    }
}

int32_t QCamera3PostProcessor::cancelQueuedFrame(uint32_t frameNumber)
{
    // A caller can own HDR or reprocess locks. This independent marker lock
    // is never held while taking either lock or delivering any callback.
    pthread_mutex_lock(&mCancelJobLock);
    bool found = false;
    for (List<uint32_t>::iterator it = mCancelpprocReqFrameList.begin();
            it != mCancelpprocReqFrameList.end(); ++it)
        found |= *it == frameNumber;
    if (!found) mCancelpprocReqFrameList.push_back(frameNumber);
    pthread_mutex_unlock(&mCancelJobLock);
    // The caller may hold HWI's result lock. Buffer/error callbacks must run
    // on the worker after this call returns, rather than reentering that lock.
    m_dataProcTh.sendCmd(CAMERA_CMD_TYPE_CANCEL_PP_FRAME, FALSE, FALSE);
    return NO_ERROR;
}

bool QCamera3PostProcessor::isQueuedFrameCancelled(uint32_t frameNumber)
{
    pthread_mutex_lock(&mCancelJobLock);
    bool cancelled = false;
    for (List<uint32_t>::iterator it = mCancelpprocReqFrameList.begin();
            it != mCancelpprocReqFrameList.end(); ++it)
        cancelled |= *it == frameNumber;
    pthread_mutex_unlock(&mCancelJobLock);
    return cancelled;
}

void QCamera3PostProcessor::cancelQueuedFrames()
{
    List<qcamera_hal3_pp_buffer_t *> inputs;
    List<qcamera_hal3_meta_pp_buffer_t *> metadata;
    List<qcamera_hal3_pp_data_t *> multi;
    List<uint32_t> frames;
    pthread_mutex_lock(&mReprocJobLock);
    pthread_mutex_lock(&mCancelJobLock);
    for (List<uint32_t>::iterator it = mCancelpprocReqFrameList.begin();
            it != mCancelpprocReqFrameList.end(); ++it) frames.push_back(*it);
    pthread_mutex_unlock(&mCancelJobLock);
    for (List<uint32_t>::iterator it = frames.begin(); it != frames.end(); ++it) {
        uint32_t frameNumber = *it;
        qcamera_hal3_pp_buffer_t *input;
        while ((input = isFrameMatched(frameNumber)) != NULL)
            inputs.push_back(input);
        qcamera_hal3_meta_pp_buffer_t *meta;
        while ((meta = isMetaMatched(frameNumber)) != NULL)
            metadata.push_back(meta);
        for (List<ReprocessBuffer>::iterator node = mReprocessNode.begin();
                node != mReprocessNode.end();) {
            if (node->reprocBuf->frameNumber == frameNumber) {
                inputs.push_back(node->reprocBuf);
                metadata.push_back(node->metaBuffer);
                node = mReprocessNode.erase(node);
            } else ++node;
        }
        qcamera_hal3_pp_data_t *job;
        while ((job = static_cast<qcamera_hal3_pp_data_t *>(
                m_inputMultiReprocQ.dequeue(matchPPJob, &frameNumber))) != NULL)
            multi.push_back(job);
    }
    // Keep each cancellation until stop: source and metadata callbacks can
    // arrive independently, including after the first cancellation command.
    pthread_mutex_unlock(&mReprocJobLock);
    for (List<qcamera_hal3_pp_buffer_t *>::iterator it = inputs.begin();
            it != inputs.end(); ++it) {
        qcamera_hal3_pp_buffer_t *input = *it;
        const bool hdr = isHdrCaptureFrame(input->frameNumber) ||
                isDiscardedHdrFrame(input->frameNumber);
        releasePPInputData(input, this);
        if (hdr) retireHdrFrame(input->frameNumber);
        // HDR's framework output belongs to the group timeout/flush path.
        // Internal source cleanup must never return that JPEG a second time.
        if (!hdr && m_pReprocChannel[0])
            m_pReprocChannel[0]->releaseOutputBuffer(input->output, input->frameNumber);
        free(input);
    }
    for (List<qcamera_hal3_meta_pp_buffer_t *>::iterator it = metadata.begin();
            it != metadata.end(); ++it) {
        releaseMetadata(*it, this);
        free(*it);
    }
    for (List<qcamera_hal3_pp_data_t *>::iterator it = multi.begin();
            it != multi.end(); ++it) {
        releaseOfflineBuffersForFrame((*it)->frameNumber);
        releaseOngoingPPData(*it, this);
        free(*it);
    }
    for (List<uint32_t>::iterator it = frames.begin(); it != frames.end(); ++it)
        eraseJpegSetting(*it);
}

/*===========================================================================
 * FUNCTION   : timeoutFrame
 *
 * DESCRIPTION: Function to handle timeouts in reprocess
 *
 * PARAMETERS :
 *   @frameNumber      : reprocess frame number that timed out
 *
 * RETURN     : NO_ERROR or valid error number
 *==========================================================================*/
int32_t QCamera3PostProcessor::timeoutFrame(uint32_t frameNumber)
{
    if (isHdrCaptureFrame(frameNumber) || isDiscardedHdrFrame(frameNumber)) {
        cancelHdrCapture(frameNumber);
        return NO_ERROR;
    }
    int32_t rc = NO_ERROR;

    if(NULL != m_pReprocChannel[0]) {
        rc = m_pReprocChannel[0]->timeoutFrame(frameNumber);
    }
    return rc;
}

/*===========================================================================
 * FUNCTION   : releaseMetaData
 *
 * DESCRIPTION: callback function to release metadata camera buffer
 *
 * PARAMETERS :
 *   @data      : ptr to post process input data
 *   @user_data : user data ptr (QCamera3Reprocessor)
 *
 * RETURN     : None
 *==========================================================================*/
void QCamera3PostProcessor::releaseMetadata(void *data, void *user_data)
{
    QCamera3PostProcessor *pme = (QCamera3PostProcessor *)user_data;
    if (NULL != pme) {
        qcamera_hal3_meta_pp_buffer_t *buf  = (qcamera_hal3_meta_pp_buffer_t *)data;
        if (buf && buf->metabuf) {
            pme->m_parent->metadataBufDone(buf->metabuf);
            free(buf->metabuf);
            buf->metabuf = NULL;
        }
    }
}

/*===========================================================================
 * FUNCTION   : releaseJpegData
 *
 * DESCRIPTION: callback function to release jpeg job node
 *
 * PARAMETERS :
 *   @data      : ptr to ongoing jpeg job data
 *   @user_data : user data ptr (QCamera3Reprocessor)
 *
 * RETURN     : None
 *==========================================================================*/
void QCamera3PostProcessor::releaseJpegData(void *data, void *user_data)
{
    QCamera3PostProcessor *pme = (QCamera3PostProcessor *)user_data;
    if (NULL != pme) {
        qcamera_hal3_jpeg_data_t *job = static_cast<qcamera_hal3_jpeg_data_t *>(data);
        if (job && job->jpeg_settings)
            pme->releaseOfflineBuffersForFrame(job->jpeg_settings->frame_number);
        // Successful JPEG callbacks return this source before calling the
        // structural releaser. Flushed/aborted queue owners have no callback.
        mm_camera_super_buf_t *source = job ?
                (job->src_reproc_frame ? job->src_reproc_frame : job->src_frame) : NULL;
        if (source && source->num_bufs == 1 && source->bufs[0] &&
                source->bufs[0]->stream_type == CAM_STREAM_TYPE_SNAPSHOT)
            pme->m_parent->freeBufferForFrame(source);
        pme->releaseJpegJobData(job);
    }
}

/*===========================================================================
 * FUNCTION   : releaseOngoingPPData
 *
 * DESCRIPTION: callback function to release ongoing postprocess job node
 *
 * PARAMETERS :
 *   @data      : ptr to onging postprocess job
 *   @user_data : user data ptr (QCamera3Reprocessor)
 *
 * RETURN     : None
 *==========================================================================*/
void QCamera3PostProcessor::releaseOngoingPPData(void *data, void *user_data)
{
    QCamera3PostProcessor *pme = (QCamera3PostProcessor *)user_data;
    if (NULL != pme) {
        qcamera_hal3_pp_data_t *pp_data = (qcamera_hal3_pp_data_t *)data;

        if (!pp_data) return;
        if (pp_data->src_frame) {
            if (pp_data->src_frame->num_bufs == 1 && pp_data->src_frame->bufs[0] &&
                    pp_data->src_frame->bufs[0]->stream_type == CAM_STREAM_TYPE_SNAPSHOT)
                pme->m_parent->freeBufferForFrame(pp_data->src_frame);
            else
                pme->releaseSuperBuf(pp_data->src_frame);
        }
        if (pp_data->reprocessed_src_frame &&
                pp_data->reprocessed_src_frame != pp_data->src_frame) {
            for (int8_t i = 0; i < pme->m_ppChannelCnt; ++i) {
                if (pme->m_pReprocChannel[i] &&
                        pme->m_pReprocChannel[i]->getMyHandle() ==
                        pp_data->reprocessed_src_frame->ch_id) {
                    pme->m_pReprocChannel[i]->bufDone(pp_data->reprocessed_src_frame);
                    break;
                }
            }
            free(pp_data->reprocessed_src_frame);
        }
        pp_data->reprocessed_src_frame = NULL;
        if (pp_data->ppOutput_jpeg_settings != pp_data->jpeg_settings)
            free(pp_data->ppOutput_jpeg_settings);
        free(pp_data->jpeg_settings);
        pp_data->jpeg_settings = pp_data->ppOutput_jpeg_settings = NULL;
        pme->releasePPJobData(pp_data);
        pme->retireHdrFrame(pp_data->frameNumber);
    }
}

/*===========================================================================
 * FUNCTION   : releaseSuperBuf
 *
 * DESCRIPTION: function to release a superbuf frame by returning back to kernel
 *
 * PARAMETERS :
 *   @super_buf : ptr to the superbuf frame
 *
 * RETURN     : None
 *==========================================================================*/
void QCamera3PostProcessor::releaseSuperBuf(mm_camera_super_buf_t *super_buf)
{
    if (NULL != super_buf) {
        if (m_parent != NULL) {
            m_parent->bufDone(super_buf);
        }
    }
}

/*===========================================================================
 * FUNCTION   : releaseOfflineBuffers
 *
 * DESCRIPTION: function to release/unmap offline buffers if any
 *
 * PARAMETERS :
 * @allBuffers : flag that asks to release all buffers or only one
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t QCamera3PostProcessor::releaseOfflineBuffers(bool allBuffers)
{
    int32_t rc = NO_ERROR;

    for (int8_t i = 0; i < m_ppChannelCnt; i++) {
        QCamera3ReprocessChannel *pChannel = m_pReprocChannel[i];
        if (pChannel != NULL ) {
            rc |= pChannel->unmapOfflineBuffers(allBuffers);
        }
    }

    return rc;
}

int32_t QCamera3PostProcessor::releaseOfflineBuffersForFrame(uint32_t frameNumber)
{
    int32_t rc = NO_ERROR;
    for (int8_t i = 0; i < m_ppChannelCnt; ++i) {
        if (m_pReprocChannel[i])
            rc |= m_pReprocChannel[i]->unmapOfflineFrame(frameNumber);
    }
    return rc;
}

/*===========================================================================
 * FUNCTION   : releaseJpegJobData
 *
 * DESCRIPTION: function to release internal resources in jpeg job struct
 *
 * PARAMETERS :
 *   @job     : ptr to jpeg job struct
 *
 * RETURN     : None
 *
 * NOTE       : original source frame need to be queued back to kernel for
 *              future use. Output buf of jpeg job need to be released since
 *              it's allocated for each job. Exif object need to be deleted.
 *==========================================================================*/
void QCamera3PostProcessor::releaseJpegJobData(qcamera_hal3_jpeg_data_t *job)
{
    ATRACE_CAMSCOPE_CALL(CAMSCOPE_HAL3_PPROC_REL_JPEG_JOB_DATA);
    int32_t rc = NO_ERROR;
    LOGD("E");
    if (NULL != job) {
        if (NULL != job->src_reproc_frame) {
            free(job->src_reproc_frame);
            job->src_reproc_frame = NULL;
        }

        if (NULL != job->src_frame) {
            // find the correct reprocess channel for the super buffer
            for (int8_t i = 0; i < m_ppChannelCnt; i++) {
                if (m_pReprocChannel[i] != NULL &&
                    m_pReprocChannel[i]->getMyHandle() == job->src_frame->ch_id) {
                    rc = m_pReprocChannel[i]->bufDone(job->src_frame);
                    if (NO_ERROR != rc) {
                        LOGE("bufDone error: %d", rc);
                    }
                    break;
                }
            }

            free(job->src_frame);
            job->src_frame = NULL;
        }

        if (NULL != job->fwk_src_buffer) {
            free(job->fwk_src_buffer);
            job->fwk_src_buffer = NULL;
        } else if (NULL != job->src_metadata) {
            m_parent->metadataBufDone(job->src_metadata);
            free(job->src_metadata);
            job->src_metadata = NULL;
        }

        if (NULL != job->fwk_frame) {
            free(job->fwk_frame);
            job->fwk_frame = NULL;
        }

        if (NULL != job->pJpegExifObj) {
            delete job->pJpegExifObj;
            job->pJpegExifObj = NULL;
        }

        if (NULL != job->jpeg_settings) {
            free(job->jpeg_settings);
            job->jpeg_settings = NULL;
        }
    }
    /* Additional trigger to process any pending jobs in the input queue */
    m_dataProcTh.sendCmd(CAMERA_CMD_TYPE_DO_NEXT_JOB, FALSE, FALSE);
    LOGD("X");
}

/*===========================================================================
 * FUNCTION   : releasePPJobData
 *
 * DESCRIPTION: function to release internal resources in p pjob struct
 *
 * PARAMETERS :
 *   @job     : ptr to pp job struct
 *
 * RETURN     : None
 *
 * NOTE       : Original source metadata buffer needs to be released and
 *              queued back to kernel for future use. src_frame, src_metadata,
 *              and fwk_src_frame structures need to be freed.
 *==========================================================================*/
void QCamera3PostProcessor::releasePPJobData(qcamera_hal3_pp_data_t *pp_job)
{
    ATRACE_CAMSCOPE_CALL(CAMSCOPE_HAL3_PPROC_REL_PP_JOB_DATA);
    LOGD("E");
    if (NULL != pp_job) {
        if (NULL != pp_job->src_frame) {
            free(pp_job->src_frame);
            if (NULL != pp_job->src_metadata) {
                m_parent->metadataBufDone(pp_job->src_metadata);
                free(pp_job->src_metadata);
            }
            pp_job->src_frame = NULL;
            pp_job->metadata = NULL;
        }

        if (NULL != pp_job->fwk_src_frame) {
            free(pp_job->fwk_src_frame);
            pp_job->fwk_src_frame = NULL;
        }
    }

    /* Additional trigger to process any pending jobs in the input queue */
    m_dataProcTh.sendCmd(CAMERA_CMD_TYPE_DO_NEXT_JOB, FALSE, FALSE);
    LOGD("X");
}

/*===========================================================================
 * FUNCTION   : getColorfmtFromImgFmt
 *
 * DESCRIPTION: function to return jpeg color format based on its image format
 *
 * PARAMETERS :
 *   @img_fmt : image format
 *
 * RETURN     : jpeg color format that can be understandable by omx lib
 *==========================================================================*/
mm_jpeg_color_format QCamera3PostProcessor::getColorfmtFromImgFmt(cam_format_t img_fmt)
{
    switch (img_fmt) {
    case CAM_FORMAT_YUV_420_NV21:
    case CAM_FORMAT_YUV_420_NV21_VENUS:
        return MM_JPEG_COLOR_FORMAT_YCRCBLP_H2V2;
    case CAM_FORMAT_YUV_420_NV21_ADRENO:
        return MM_JPEG_COLOR_FORMAT_YCRCBLP_H2V2;
    case CAM_FORMAT_YUV_420_NV12:
    case CAM_FORMAT_YUV_420_NV12_VENUS:
        return MM_JPEG_COLOR_FORMAT_YCBCRLP_H2V2;
    case CAM_FORMAT_YUV_420_YV12:
        return MM_JPEG_COLOR_FORMAT_YCBCRLP_H2V2;
    case CAM_FORMAT_YUV_422_NV61:
        return MM_JPEG_COLOR_FORMAT_YCRCBLP_H2V1;
    case CAM_FORMAT_YUV_422_NV16:
        return MM_JPEG_COLOR_FORMAT_YCBCRLP_H2V1;
    case CAM_FORMAT_Y_ONLY:
        return MM_JPEG_COLOR_FORMAT_MONOCHROME;
    default:
        return MM_JPEG_COLOR_FORMAT_YCRCBLP_H2V2;
    }
}

/*===========================================================================
 * FUNCTION   : getJpegImgTypeFromImgFmt
 *
 * DESCRIPTION: function to return jpeg encode image type based on its image format
 *
 * PARAMETERS :
 *   @img_fmt : image format
 *
 * RETURN     : return jpeg source image format (YUV or Bitstream)
 *==========================================================================*/
mm_jpeg_format_t QCamera3PostProcessor::getJpegImgTypeFromImgFmt(cam_format_t img_fmt)
{
    switch (img_fmt) {
    case CAM_FORMAT_YUV_420_NV21:
    case CAM_FORMAT_YUV_420_NV21_ADRENO:
    case CAM_FORMAT_YUV_420_NV12:
    case CAM_FORMAT_YUV_420_NV12_VENUS:
    case CAM_FORMAT_YUV_420_NV21_VENUS:
    case CAM_FORMAT_YUV_420_YV12:
    case CAM_FORMAT_YUV_422_NV61:
    case CAM_FORMAT_YUV_422_NV16:
        return MM_JPEG_FMT_YUV;
    default:
        return MM_JPEG_FMT_YUV;
    }
}

/*===========================================================================
 * FUNCTION   : encodeFWKData
 *
 * DESCRIPTION: function to prepare encoding job information and send to
 *              mm-jpeg-interface to do the encoding job
 *
 * PARAMETERS :
 *   @jpeg_job_data : ptr to a struct saving job related information
 *   @needNewSess   : flag to indicate if a new jpeg encoding session need
 *                    to be created. After creation, this flag will be toggled
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t QCamera3PostProcessor::encodeFWKData(qcamera_hal3_jpeg_data_t *jpeg_job_data,
        uint8_t &needNewSess)
{
    LOGD("E");
    int32_t ret = NO_ERROR;
    mm_jpeg_job_t jpg_job;
    uint32_t jobId = 0;
    qcamera_fwk_input_pp_data_t *recvd_frame = NULL;
    metadata_buffer_t *metadata = NULL;
    jpeg_settings_t *jpeg_settings = NULL;
    QCamera3HardwareInterface* hal_obj = NULL;
    mm_jpeg_debug_exif_params_t *exif_debug_params = NULL;
    bool needJpegExifRotation = false;

    if (NULL == jpeg_job_data) {
        LOGE("Invalid jpeg job");
        return BAD_VALUE;
    }

    recvd_frame = jpeg_job_data->fwk_frame;
    if (NULL == recvd_frame) {
        LOGE("Invalid input buffer");
        return BAD_VALUE;
    }

    metadata = jpeg_job_data->metadata;
    if (NULL == metadata) {
        LOGE("Invalid metadata buffer");
        return BAD_VALUE;
    }

    jpeg_settings = jpeg_job_data->jpeg_settings;
    if (NULL == jpeg_settings) {
        LOGE("Invalid jpeg settings buffer");
        return BAD_VALUE;
    }

    if ((NULL != jpeg_job_data->fwk_frame) && (NULL != jpeg_job_data->src_frame)) {
        LOGE("Unsupported case both framework and camera source buffers are invalid!");
        return BAD_VALUE;
    }

    hal_obj = (QCamera3HardwareInterface*)m_parent->mUserData;
    if (hal_obj == NULL) {
        LOGE("hal_obj is NULL, Error");
        return BAD_VALUE;
    }

    if (mJpegClientHandle <= 0) {
        LOGE("Error: bug here, mJpegClientHandle is 0");
        return UNKNOWN_ERROR;
    }

    cam_dimension_t src_dim;
    memset(&src_dim, 0, sizeof(cam_dimension_t));
    src_dim.width = recvd_frame->reproc_config.input_stream_dim.width;
    src_dim.height = recvd_frame->reproc_config.input_stream_dim.height;

    cam_dimension_t dst_dim;
    memset(&dst_dim, 0, sizeof(cam_dimension_t));
    dst_dim.width = recvd_frame->reproc_config.output_stream_dim.width;
    dst_dim.height = recvd_frame->reproc_config.output_stream_dim.height;

    cam_rect_t crop;
    memset(&crop, 0, sizeof(cam_rect_t));
    //TBD_later - Zoom event removed in stream
    //main_stream->getCropInfo(crop);
    // Set JPEG encode crop in reprocess frame metadata
    // If this JPEG crop info exist, encoder should do cropping
    IF_META_AVAILABLE(cam_stream_crop_info_t, jpeg_crop,
            CAM_INTF_PARM_JPEG_ENCODE_CROP, metadata) {
        memcpy(&crop, &(jpeg_crop->crop), sizeof(cam_rect_t));
    }

    // Set JPEG encode crop in reprocess frame metadata
    // If this JPEG scale info exist, encoder should do scaling
    IF_META_AVAILABLE(cam_dimension_t, scale_dim,
            CAM_INTF_PARM_JPEG_SCALE_DIMENSION, metadata) {
        if (scale_dim->width != 0 && scale_dim->height != 0) {
            dst_dim.width = scale_dim->width;
            dst_dim.height = scale_dim->height;
        }
    }

    needJpegExifRotation = (hal_obj->needJpegExifRotation() || !needsReprocess(recvd_frame));

    // If EXIF rotation metadata is added and used to match the JPEG orientation,
    // it means CPP rotation is not involved, whether it is because CPP does not
    // support rotation, or the reprocessed frame is not sent to CPP.
    // Override CAM_INTF_PARM_ROTATION to 0 to avoid wrong CPP rotation info
    // to be filled in to JPEG metadata.
    if (needJpegExifRotation) {
        cam_rotation_info_t rotation_info;
        memset(&rotation_info, 0, sizeof(rotation_info));
        rotation_info.rotation = ROTATE_0;
        rotation_info.streamId = 0;
        ADD_SET_PARAM_ENTRY_TO_BATCH(metadata, CAM_INTF_PARM_ROTATION, rotation_info);
    }

    LOGH("Need new session?:%d jpeg_orientation %d needJpegExifRotation %d useExifRotation %d",
            needNewSess, jpeg_settings->jpeg_orientation, needJpegExifRotation,
            hal_obj->useExifRotation());
    if (needNewSess) {
        //creating a new session, so we must destroy the old one
        if ( 0 < mJpegSessionId ) {
            ret = mJpegHandle.destroy_session(mJpegSessionId);
            if (ret != NO_ERROR) {
                LOGE("Error destroying an old jpeg encoding session, id = %d",
                       mJpegSessionId);
                return ret;
            }
            mJpegSessionId = 0;
        }
        // create jpeg encoding session
        mm_jpeg_encode_params_t encodeParam;
        memset(&encodeParam, 0, sizeof(mm_jpeg_encode_params_t));
        getFWKJpegEncodeConfig(encodeParam, recvd_frame, jpeg_settings);
        QCamera3StreamMem *memObj = (QCamera3StreamMem *)(recvd_frame->input_buffer.mem_info);
        if (NULL == memObj) {
            LOGE("Memeory Obj of main frame is NULL");
            return NO_MEMORY;
        }
        // clean and invalidate cache ops through mem obj of the frame
        memObj->cleanInvalidateCache(recvd_frame->input_buffer.buf_idx);

        LOGH("#src bufs:%d # tmb bufs:%d #dst_bufs:%d",
                     encodeParam.num_src_bufs,encodeParam.num_tmb_bufs,encodeParam.num_dst_bufs);
        if (!needJpegExifRotation &&
            (jpeg_settings->jpeg_orientation == 90 ||
            jpeg_settings->jpeg_orientation == 270)) {
            // swap src width and height, stride and scanline due to rotation
            encodeParam.main_dim.src_dim.width = src_dim.height;
            encodeParam.main_dim.src_dim.height = src_dim.width;
            encodeParam.thumb_dim.src_dim.width = src_dim.height;
            encodeParam.thumb_dim.src_dim.height = src_dim.width;

            int32_t temp = encodeParam.src_main_buf[0].offset.mp[0].stride;
            encodeParam.src_main_buf[0].offset.mp[0].stride =
                encodeParam.src_main_buf[0].offset.mp[0].scanline;
            encodeParam.src_main_buf[0].offset.mp[0].scanline = temp;

            temp = encodeParam.src_thumb_buf[0].offset.mp[0].stride;
            encodeParam.src_thumb_buf[0].offset.mp[0].stride =
                encodeParam.src_thumb_buf[0].offset.mp[0].scanline;
            encodeParam.src_thumb_buf[0].offset.mp[0].scanline = temp;
        } else {
            encodeParam.main_dim.src_dim = src_dim;
            encodeParam.thumb_dim.src_dim = src_dim;
        }
        encodeParam.main_dim.dst_dim = dst_dim;
        encodeParam.thumb_dim.dst_dim = jpeg_settings->thumbnail_size;

        if (!hal_obj->useExifRotation() && needJpegExifRotation) {
            encodeParam.rotation = jpeg_settings->jpeg_orientation;
            encodeParam.thumb_rotation = jpeg_settings->jpeg_orientation;
        }

        LOGI("Src Buffer cnt = %d, res = %dX%d len = %d rot = %d "
            "src_dim = %dX%d dst_dim = %dX%d",
            encodeParam.num_src_bufs,
            encodeParam.src_main_buf[0].offset.mp[0].stride,
            encodeParam.src_main_buf[0].offset.mp[0].scanline,
            encodeParam.src_main_buf[0].offset.frame_len,
            encodeParam.rotation,
            src_dim.width, src_dim.height,
            dst_dim.width, dst_dim.height);
        LOGI("Src THUMB buf_cnt = %d, res = %dX%d len = %d rot = %d "
            "src_dim = %dX%d, dst_dim = %dX%d",
            encodeParam.num_tmb_bufs,
            encodeParam.src_thumb_buf[0].offset.mp[0].stride,
            encodeParam.src_thumb_buf[0].offset.mp[0].scanline,
            encodeParam.src_thumb_buf[0].offset.frame_len,
            encodeParam.thumb_rotation,
            encodeParam.thumb_dim.src_dim.width,
            encodeParam.thumb_dim.src_dim.height,
            encodeParam.thumb_dim.dst_dim.width,
            encodeParam.thumb_dim.dst_dim.height);

        LOGH("#src bufs:%d # tmb bufs:%d #dst_bufs:%d",
                     encodeParam.num_src_bufs,encodeParam.num_tmb_bufs,encodeParam.num_dst_bufs);

        ret = mJpegHandle.create_session(mJpegClientHandle, &encodeParam, &mJpegSessionId);
        if (ret != NO_ERROR) {
            LOGE("Error creating a new jpeg encoding session, ret = %d", ret);
            return ret;
        }
        needNewSess = FALSE;
    }

    // Fill in new job
    memset(&jpg_job, 0, sizeof(mm_jpeg_job_t));
    jpg_job.job_type = JPEG_JOB_TYPE_ENCODE;
    jpg_job.encode_job.session_id = mJpegSessionId;
    jpg_job.encode_job.src_index = 0;
    jpg_job.encode_job.dst_index = 0;

    // Set main dim job parameters and handle rotation
    if (!needJpegExifRotation && (jpeg_settings->jpeg_orientation == 90 ||
            jpeg_settings->jpeg_orientation == 270)) {

        jpg_job.encode_job.main_dim.src_dim.width = src_dim.height;
        jpg_job.encode_job.main_dim.src_dim.height = src_dim.width;

        jpg_job.encode_job.main_dim.dst_dim.width = dst_dim.height;
        jpg_job.encode_job.main_dim.dst_dim.height = dst_dim.width;

        jpg_job.encode_job.main_dim.crop.width = crop.height;
        jpg_job.encode_job.main_dim.crop.height = crop.width;
        jpg_job.encode_job.main_dim.crop.left = crop.top;
        jpg_job.encode_job.main_dim.crop.top = crop.left;
    } else {
        jpg_job.encode_job.main_dim.src_dim = src_dim;
        jpg_job.encode_job.main_dim.dst_dim = dst_dim;
        jpg_job.encode_job.main_dim.crop = crop;
    }

    // get 3a sw version info
    cam_q3a_version_t sw_version;
    memset(&sw_version, 0, sizeof(sw_version));
    if (hal_obj)
        hal_obj->get3AVersion(sw_version);

    // get exif data
    QCamera3Exif *pJpegExifObj = getExifData(metadata, jpeg_settings,
            (needJpegExifRotation && hal_obj->useExifRotation()));
    jpeg_job_data->pJpegExifObj = pJpegExifObj;
    if (pJpegExifObj != NULL) {
        jpg_job.encode_job.exif_info.exif_data = pJpegExifObj->getEntries();
        jpg_job.encode_job.exif_info.numOfEntries =
            pJpegExifObj->getNumOfEntries();
        jpg_job.encode_job.exif_info.debug_data.sw_3a_version[0] =
            sw_version.major_version;
        jpg_job.encode_job.exif_info.debug_data.sw_3a_version[1] =
            sw_version.minor_version;
        jpg_job.encode_job.exif_info.debug_data.sw_3a_version[2] =
            sw_version.patch_version;
        jpg_job.encode_job.exif_info.debug_data.sw_3a_version[3] =
            sw_version.new_feature_des;
    }

    if (!hal_obj->useExifRotation() && needJpegExifRotation) {
        jpg_job.encode_job.rotation= jpeg_settings->jpeg_orientation;
    }

    // thumbnail dim
    LOGH("Thumbnail needed:%d", m_bThumbnailNeeded);
    if (m_bThumbnailNeeded == TRUE) {
        jpg_job.encode_job.thumb_dim.dst_dim =
                jpeg_settings->thumbnail_size;

        if (!needJpegExifRotation && (jpeg_settings->jpeg_orientation == 90 ||
                jpeg_settings->jpeg_orientation == 270)) {
            //swap the thumbnail destination width and height if it has
            //already been rotated
            int temp = jpg_job.encode_job.thumb_dim.dst_dim.width;
            jpg_job.encode_job.thumb_dim.dst_dim.width =
                    jpg_job.encode_job.thumb_dim.dst_dim.height;
            jpg_job.encode_job.thumb_dim.dst_dim.height = temp;

            jpg_job.encode_job.thumb_dim.src_dim.width = src_dim.height;
            jpg_job.encode_job.thumb_dim.src_dim.height = src_dim.width;

            jpg_job.encode_job.thumb_dim.crop.width = crop.height;
            jpg_job.encode_job.thumb_dim.crop.height = crop.width;
            jpg_job.encode_job.thumb_dim.crop.left = crop.top;
            jpg_job.encode_job.thumb_dim.crop.top = crop.left;
        } else {
        jpg_job.encode_job.thumb_dim.src_dim = src_dim;
        jpg_job.encode_job.thumb_dim.crop = crop;
        }
        jpg_job.encode_job.thumb_index = 0;
        LOGI("Thumbnail idx = %d src w/h (%dx%d), dst w/h (%dx%d)",
                jpg_job.encode_job.thumb_index,
                jpg_job.encode_job.thumb_dim.src_dim.width,
                jpg_job.encode_job.thumb_dim.src_dim.height,
                jpg_job.encode_job.thumb_dim.dst_dim.width,
                jpg_job.encode_job.thumb_dim.dst_dim.height);
    }

    LOGI("Main image idx = %d src w/h (%dx%d), dst w/h (%dx%d) rot = %d",
            jpg_job.encode_job.src_index,
            jpg_job.encode_job.main_dim.src_dim.width,
            jpg_job.encode_job.main_dim.src_dim.height,
            jpg_job.encode_job.main_dim.dst_dim.width,
            jpg_job.encode_job.main_dim.dst_dim.height,
            jpg_job.encode_job.rotation);
    // Allocate for a local copy of debug parameters
    jpg_job.encode_job.cam_exif_params.debug_params =
            (mm_jpeg_debug_exif_params_t *) malloc (sizeof(mm_jpeg_debug_exif_params_t));
    if (!jpg_job.encode_job.cam_exif_params.debug_params) {
        LOGE("Out of Memory. Allocation failed for 3A debug exif params");
        return NO_MEMORY;
    }

    memset(jpg_job.encode_job.cam_exif_params.debug_params, 0,
            sizeof(mm_jpeg_debug_exif_params_t));
    exif_debug_params = jpg_job.encode_job.cam_exif_params.debug_params;

    jpg_job.encode_job.mobicat_mask = hal_obj->getMobicatMask();

    if (metadata != NULL) {
        // Fill in the metadata passed as parameter
        jpg_job.encode_job.p_metadata = metadata;

        // Fill in exif debug data
        if (exif_debug_params) {
            // AE
            IF_META_AVAILABLE(cam_ae_exif_debug_t, ae_exif_debug_params,
                    CAM_INTF_META_EXIF_DEBUG_AE, metadata) {
                memcpy(&exif_debug_params->ae_debug_params, ae_exif_debug_params,
                        sizeof(cam_ae_exif_debug_t));
                exif_debug_params->ae_debug_params_valid = TRUE;
                jpg_job.encode_job.p_metadata->is_valid[CAM_INTF_META_EXIF_DEBUG_AE] = TRUE;
            }
            // AWB
            IF_META_AVAILABLE(cam_awb_exif_debug_t, awb_exif_debug_params,
                    CAM_INTF_META_EXIF_DEBUG_AWB, metadata) {
                memcpy(&exif_debug_params->awb_debug_params, awb_exif_debug_params,
                        sizeof(cam_awb_exif_debug_t));
                exif_debug_params->awb_debug_params_valid = TRUE;
                jpg_job.encode_job.p_metadata->is_valid[CAM_INTF_META_EXIF_DEBUG_AWB] = TRUE;
            }
            // AF
            IF_META_AVAILABLE(cam_af_exif_debug_t, af_exif_debug_params,
                    CAM_INTF_META_EXIF_DEBUG_AF, metadata) {
                memcpy(&exif_debug_params->af_debug_params, af_exif_debug_params,
                        sizeof(cam_af_exif_debug_t));
                exif_debug_params->af_debug_params_valid = TRUE;
                jpg_job.encode_job.p_metadata->is_valid[CAM_INTF_META_EXIF_DEBUG_AF] = TRUE;
            }
            // ASD
            IF_META_AVAILABLE(cam_asd_exif_debug_t, asd_exif_debug_params,
                    CAM_INTF_META_EXIF_DEBUG_ASD, metadata) {
                memcpy(&exif_debug_params->asd_debug_params, asd_exif_debug_params,
                        sizeof(cam_asd_exif_debug_t));
                exif_debug_params->asd_debug_params_valid = TRUE;
                jpg_job.encode_job.p_metadata->is_valid[CAM_INTF_META_EXIF_DEBUG_ASD] = TRUE;
            }
            // STATS
            IF_META_AVAILABLE(cam_stats_buffer_exif_debug_t, stats_exif_debug_params,
                    CAM_INTF_META_EXIF_DEBUG_STATS, metadata) {
                memcpy(&exif_debug_params->stats_debug_params, stats_exif_debug_params,
                        sizeof(cam_stats_buffer_exif_debug_t));
                exif_debug_params->stats_debug_params_valid = TRUE;
                jpg_job.encode_job.p_metadata->is_valid[CAM_INTF_META_EXIF_DEBUG_STATS] = TRUE;
            }
            // BE STATS
            IF_META_AVAILABLE(cam_bestats_buffer_exif_debug_t, bestats_exif_debug_params,
                    CAM_INTF_META_EXIF_DEBUG_BESTATS, metadata) {
                memcpy(&exif_debug_params->bestats_debug_params, bestats_exif_debug_params,
                        sizeof(cam_bestats_buffer_exif_debug_t));
                exif_debug_params->bestats_debug_params_valid = TRUE;
                jpg_job.encode_job.p_metadata->is_valid[CAM_INTF_META_EXIF_DEBUG_BESTATS] = TRUE;
            }
            // BHIST
            IF_META_AVAILABLE(cam_bhist_buffer_exif_debug_t, bhist_exif_debug_params,
                    CAM_INTF_META_EXIF_DEBUG_BHIST, metadata) {
                memcpy(&exif_debug_params->bhist_debug_params, bhist_exif_debug_params,
                        sizeof(cam_bhist_buffer_exif_debug_t));
                exif_debug_params->bhist_debug_params_valid = TRUE;
                jpg_job.encode_job.p_metadata->is_valid[CAM_INTF_META_EXIF_DEBUG_BHIST] = TRUE;
            }
            // Q3A
            IF_META_AVAILABLE(cam_q3a_tuning_info_t, q3a_tuning_exif_debug_params,
                    CAM_INTF_META_EXIF_DEBUG_3A_TUNING, metadata) {
                memcpy(&exif_debug_params->q3a_tuning_debug_params, q3a_tuning_exif_debug_params,
                        sizeof(cam_q3a_tuning_info_t));
                exif_debug_params->q3a_tuning_debug_params_valid = TRUE;
                jpg_job.encode_job.p_metadata->is_valid[CAM_INTF_META_EXIF_DEBUG_3A_TUNING] = TRUE;
            }
        }
    } else {
       LOGW("Metadata is null");
    }

    // Multi image info
    if (hal_obj->isDeviceLinked() == TRUE) {
        jpg_job.encode_job.multi_image_info.type = MM_JPEG_TYPE_JPEG;
        jpg_job.encode_job.multi_image_info.num_of_images = 1;
        jpg_job.encode_job.multi_image_info.enable_metadata = 1;
        if (hal_obj->isMainCamera() == TRUE) {
            jpg_job.encode_job.multi_image_info.is_primary = 1;
        } else {
            jpg_job.encode_job.multi_image_info.is_primary = 0;
        }
    }

    jpg_job.encode_job.hal_version = CAM_HAL_V3;

    //Start jpeg encoding
    ret = mJpegHandle.start_job(&jpg_job, &jobId);
    if (jpg_job.encode_job.cam_exif_params.debug_params) {
        free(jpg_job.encode_job.cam_exif_params.debug_params);
    }
    if (ret == NO_ERROR) {
        // remember job info
        jpeg_job_data->jobId = jobId;
    }

    LOGD("X");
    return ret;
}

/*===========================================================================
 * FUNCTION   : encodeData
 *
 * DESCRIPTION: function to prepare encoding job information and send to
 *              mm-jpeg-interface to do the encoding job
 *
 * PARAMETERS :
 *   @jpeg_job_data : ptr to a struct saving job related information
 *   @needNewSess   : flag to indicate if a new jpeg encoding session need
 *                    to be created. After creation, this flag will be toggled
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t QCamera3PostProcessor::encodeData(qcamera_hal3_jpeg_data_t *jpeg_job_data,
                          uint8_t &needNewSess)
{
    ATRACE_CAMSCOPE_CALL(CAMSCOPE_HAL3_PPROC_ENCODEDATA);
    LOGD("E");
    int32_t ret = NO_ERROR;
    mm_jpeg_job_t jpg_job;
    uint32_t jobId = 0;
    QCamera3Stream *main_stream = NULL;
    mm_camera_buf_def_t *main_frame = NULL;
    cam_stream_parm_buffer_t param;
    QCamera3Channel *srcChannel = NULL;
    mm_camera_super_buf_t *recvd_frame = NULL;
    metadata_buffer_t *metadata = NULL;
    jpeg_settings_t *jpeg_settings = NULL;
    QCamera3HardwareInterface* hal_obj = NULL;
    mm_jpeg_debug_exif_params_t *exif_debug_params = NULL;
    if (m_parent != NULL) {
        hal_obj = (QCamera3HardwareInterface*)m_parent->mUserData;
        if (hal_obj == NULL) {
            LOGE("hal_obj is NULL, Error");
            return BAD_VALUE;
        }
    } else {
        LOGE("m_parent is NULL, Error");
        return BAD_VALUE;
    }
    bool needJpegExifRotation = false;

    recvd_frame = jpeg_job_data->src_frame;
    metadata = jpeg_job_data->metadata;
    jpeg_settings = jpeg_job_data->jpeg_settings;

    LOGD("encoding bufIndex: %u",
        jpeg_job_data->src_frame->bufs[0]->buf_idx);

    QCamera3Channel *pChannel = NULL;
    // first check picture channel
    if ((is_dual_camera_by_handle(m_parent->getMyHandle())
        && ((get_main_camera_handle(m_parent->getMyHandle()) == recvd_frame->ch_id)
        || (get_aux_camera_handle(m_parent->getMyHandle()) == recvd_frame->ch_id)))
        || (m_parent->getMyHandle() == recvd_frame->ch_id)) {
        pChannel = m_parent;
    }
    // check reprocess channel if not found
    if (pChannel == NULL) {
        for (int8_t i = 0; i < m_ppChannelCnt; i++) {
            if (m_pReprocChannel[i] != NULL &&
                m_pReprocChannel[i]->getMyHandle() == recvd_frame->ch_id) {
                pChannel = m_pReprocChannel[i];
                break;
            }
        }
    }

    srcChannel = pChannel;

    if (srcChannel == NULL) {
        LOGE("No corresponding channel (ch_id = %d) exist, return here",
               recvd_frame->ch_id);
        return BAD_VALUE;
    }

    // find snapshot frame and thumnail frame
    //Note: In this version we will receive only snapshot frame.
    for (uint32_t i = 0; i < recvd_frame->num_bufs; i++) {
        QCamera3Stream *srcStream = NULL;
        srcStream = srcChannel->getStreamByHandle(recvd_frame->bufs[i]->stream_id);
        if (srcStream != NULL) {
            switch (srcStream->getMyType()) {
            case CAM_STREAM_TYPE_SNAPSHOT:
            case CAM_STREAM_TYPE_OFFLINE_PROC:
                main_stream = srcStream;
                main_frame = recvd_frame->bufs[i];
                break;
            default:
                break;
            }
        }
    }

    if(NULL == main_frame){
       LOGE("Main frame is NULL");
       return BAD_VALUE;
    }

    QCamera3StreamMem *memObj = (QCamera3StreamMem *)main_frame->mem_info;
    if (NULL == memObj) {
        LOGE("Memeory Obj of main frame is NULL");
        return NO_MEMORY;
    }

    if (jpeg_settings && jpeg_settings->native_still_mode == OPPO_NATIVE_REAR_BEAUTY) {
        QCamera3HdrAlgo::Image image;
        QCamera3StreamMem *memory = nullptr;
        int32_t beautyStatus = -ENOTSUP;
        if (getHdrImage(recvd_frame, image, memory)) {
            QCamera3StockBeauty::Options options;
            const int32_t strength = jpeg_settings->native_beauty_strength;
            if (strength >= 0 && strength <= 100) {
                options.soften = strength;
                options.softenSharp = strength;
                options.skinBright = strength;
                // Style is an independent stock feature; strength must not
                // select a guessed skin-style preset.
                options.skinStyle = 0;
                beautyStatus = QCamera3StockBeauty::process(image, options);
                if (beautyStatus == NO_ERROR) {
                    beautyStatus = memory->cleanCache(main_frame->buf_idx);
                    if (beautyStatus != NO_ERROR) return beautyStatus;
                }
            } else beautyStatus = -EINVAL;
        }
        nativeAlgorithmTrace(jpeg_settings, "ARCRLP", beautyStatus,
                jpeg_settings->frame_number, 1,
                static_cast<QCamera3HardwareInterface *>(m_parent->mUserData));
    }
    // Graphical watermark / sticker / mirror on the real JPEG edit surface: the
    // unrotated NV21 frame this path is about to encode. The rectangles are
    // mapped back from the viewed orientation, so preview and photo agree for
    // the same request. A bypass (no asset, no mirror) leaves the bytes alone.
    if (jpeg_settings && (jpeg_settings->native_stages.watermark_asset_length > 0 ||
            jpeg_settings->native_stages.sticker_asset_length > 0 ||
            jpeg_settings->native_stages.mirror_preview)) {
        QCamera3HdrAlgo::Image image;
        QCamera3StreamMem *memory = nullptr;
        int32_t assetStatus = -ENODATA;
        int32_t assetApplied = 0;
        if (getHdrImage(recvd_frame, image, memory)) {
            QCamera3PostprocessAssets assets;
            QCamera3PostprocessAssets::Options options;
            options.enable = 1;
            options.mirror = jpeg_settings->native_stages.mirror_preview;
            options.frameNumber = jpeg_settings->framework_frame_number_valid ?
                    jpeg_settings->framework_frame_number : jpeg_settings->frame_number;
            // 1 MiB each, and they must not live on the stack: this poster runs
            // on the postprocessing worker thread with the default (1 MiB)
            // pthread stack, while encodeData() already carries large value
            // locals (mm_jpeg_job_t alone is 80 KB). A frame that large
            // overruns the worker stack and the first memset past the stack top
            // faults (SIGSEGV on the first still capture).
            std::vector<uint8_t> watermarkStorage(
                    QCamera3PostprocessAssets::MAX_ASSET_BYTES, 0);
            std::vector<uint8_t> stickerStorage(
                    QCamera3PostprocessAssets::MAX_ASSET_BYTES, 0);
            QCamera3PostprocessAssets::Asset watermark;
            QCamera3PostprocessAssets::Asset sticker;
            bool haveWatermark = false, haveSticker = false;
            if (jpeg_settings->native_stages.watermark_asset_length > 0) {
                size_t length = 0;
                const int32_t load = QCamera3PostprocessAssets::loadAsset(
                        jpeg_settings->native_stages.watermark_asset, watermarkStorage.data(),
                        watermarkStorage.size(), length, watermark);
                if (load == NO_ERROR) { options.watermark = &watermark; haveWatermark = true; }
                else assetStatus = load;
            }
            if (jpeg_settings->native_stages.sticker_asset_length > 0) {
                size_t length = 0;
                const int32_t load = QCamera3PostprocessAssets::loadAsset(
                        jpeg_settings->native_stages.sticker_asset, stickerStorage.data(),
                        stickerStorage.size(), length, sticker);
                if (load == NO_ERROR) {
                    options.sticker = &sticker;
                    haveSticker = true;
                    if (jpeg_settings->native_stages.sticker_rect_valid) {
                        options.stickerRect = {jpeg_settings->native_stages.sticker_rect[0],
                                jpeg_settings->native_stages.sticker_rect[1],
                                jpeg_settings->native_stages.sticker_rect[2],
                                jpeg_settings->native_stages.sticker_rect[3]};
                    }
                } else assetStatus = load;
            }
            if ((haveWatermark || haveSticker || options.mirror) &&
                    QCamera3PostprocessAssets::validOptions(options)) {
                QCamera3PostprocessAssets::Result result;
                const int32_t rotation = jpeg_settings->jpeg_orientation;
                assetStatus = assets.processJpegEdit(image, image, rotation, options, result);
                assetApplied = result.applied;
                if (assetStatus == NO_ERROR && result.applied != 0) {
                    assetStatus = memory->cleanCache(main_frame->buf_idx);
                    if (assetStatus != NO_ERROR) return assetStatus;
                }
            }
        }
        if (assetStatus != NO_ERROR || assetApplied != 0)
            nativeAlgorithmTrace(jpeg_settings, "PostprocessAssets", assetStatus,
                    jpeg_settings->frame_number, 1, hal_obj, assetApplied);
    }

    // Stage evidence recorded on the still this path really produced: lens
    // occlusion, the scene-conditional brightness / low-light macro decision and
    // the EIS-aware still selection. They change no pixel; each records its own
    // real status and refuses when its real inputs are absent.
    if (jpeg_settings) {
        QCamera3HdrAlgo::Image evidence;
        QCamera3StreamMem *evidenceMemory = nullptr;
        if (getHdrImage(recvd_frame, evidence, evidenceMemory)) {
            const uint32_t evidenceFrame = jpeg_settings->framework_frame_number_valid ?
                    jpeg_settings->framework_frame_number : jpeg_settings->frame_number;
            // Multi-frame flash plan: real decoded flash presence, the real AE
            // state of this frame and the real LED-on bound. The stage is a
            // planner: it never submits a request and never changes the capture,
            // so a refusal (an undecoded bound or calibration) is recorded and
            // the still keeps its existing flash behaviour.
            {
                bool hasFlash = false;
                char flashNode[QCAMERA_MAX_FILEPATH_LENGTH] = {};
                QCamera3HardwareInterface::getFlashInfo(hal_obj->getCameraID(), hasFlash,
                        flashNode);
                QCamera3FlashSequence::Capability flashCapability;
                flashCapability.flashAvailable = hasFlash ? 1 : 0;
                flashCapability.dualLed = 0;
                QCamera3FlashSequence::AeState flashAe;
                IF_META_AVAILABLE(int32_t, flashState, CAM_INTF_META_FLASH_STATE, metadata) {
                    // Real reported flash state: FIRED means the LED really lit
                    // for this frame, everything else is not a flash decision.
                    flashAe.flashRequired = *flashState == CAM_FLASH_STATE_FIRED ? 1 : 0;
                }
                IF_META_AVAILABLE(uint32_t, flashAeState, CAM_INTF_META_AEC_STATE, metadata) {
                    flashAe.aeState = int32_t(*flashAeState);
                }
                IF_META_AVAILABLE(int64_t, flashExposure,
                        CAM_INTF_META_SENSOR_EXPOSURE_TIME, metadata) {
                    flashAe.ambientExposureNs = *flashExposure;
                }
                IF_META_AVAILABLE(int32_t, flashIso, CAM_INTF_META_SENSOR_SENSITIVITY,
                        metadata) {
                    flashAe.ambientIso = *flashIso;
                }
                QCamera3FlashSequence::Options flashOptions;
                flashOptions.flashMode = hasFlash ?
                        QCamera3FlashSequence::FLASH_MODE_SINGLE :
                        QCamera3FlashSequence::FLASH_MODE_OFF;
                flashOptions.frameNumber = evidenceFrame;
                QCamera3FlashSequence::Result flashResult;
                QCamera3FlashSequence::Plan flashPlan;
                int32_t flashStatus = -ENOTSUP;
                if (QCamera3FlashSequence::validCapability(flashCapability) &&
                        QCamera3FlashSequence::validOptions(flashOptions))
                    flashStatus = QCamera3FlashSequence::plan(flashCapability, flashAe, NULL,
                            flashOptions, flashResult, flashPlan);
                // A plan with no real LED-on bound refuses (-ENODATA): the
                // decoded bound is not present in this tree, so the refusal is
                // the honest outcome and is recorded with applied 0.
                nativeAlgorithmTrace(jpeg_settings, "FlashSequence", flashStatus,
                        jpeg_settings->frame_number, 0, hal_obj, 0);
            }

            // Scene-conditional brightness / low-light macro over the real
            // frame statistics. The decoded QCameraParameters brightness range
            // is not present in this tree, so the config refuses and only the
            // measured statistics are recorded; no target is invented.
            {
                QCamera3SceneBrightness sceneBrightness;
                QCamera3SceneBrightness::Stats brightnessStats;
                IF_META_AVAILABLE(cam_hist_stats_t, brightnessHistogram,
                        CAM_INTF_META_HISTOGRAM, metadata) {
                    if (brightnessHistogram->type == CAM_HISTOGRAM_TYPE_YUV) {
                        brightnessStats.lumaHistogram =
                                brightnessHistogram->yuv_stats.hist_buf;
                        uint64_t total = 0;
                        for (uint32_t i = 0; i < QCamera3SceneBrightness::kHistogramBins; ++i)
                            total += brightnessHistogram->yuv_stats.hist_buf[i];
                        brightnessStats.histogramTotal =
                                total <= UINT32_MAX ? uint32_t(total) : 0;
                    }
                }
                int32_t meanMilli = -1;
                if (QCamera3SceneBrightness::histogramMeanLumaMilli(brightnessStats,
                        meanMilli)) brightnessStats.meanLumaMilli = meanMilli;
                QCamera3SceneBrightness::Config brightnessConfig;
                QCamera3SceneBrightness::Result brightnessResult;
                QCamera3SceneBrightness::Decision brightnessDecision;
                int32_t brightnessStatus = -ENOTSUP;
                if (QCamera3SceneBrightness::validStats(brightnessStats) &&
                        QCamera3SceneBrightness::validConfig(brightnessConfig) &&
                        QCamera3SceneBrightness::usableStats(brightnessStats))
                    brightnessStatus = sceneBrightness.decide(brightnessStats,
                            brightnessConfig, evidenceFrame, brightnessResult,
                            brightnessDecision);
                else if (QCamera3SceneBrightness::validStats(brightnessStats))
                    brightnessStatus = sceneBrightness.idle(evidenceFrame, brightnessConfig,
                            brightnessResult, brightnessDecision);
                if (brightnessStatus != NO_ERROR || brightnessResult.applied != 0)
                    nativeAlgorithmTrace(jpeg_settings, "SceneBrightness", brightnessStatus,
                            jpeg_settings->frame_number, 0, hal_obj,
                            brightnessResult.applied);
            }

            QCamera3Occlusion occlusion;
            QCamera3Occlusion::Options occlusionOptions;
            occlusionOptions.frameNumber = evidenceFrame;
            QCamera3Occlusion::Result occlusionResult;
            int32_t occlusionStatus = -ENOTSUP;
            if (QCamera3Occlusion::validOptions(occlusionOptions))
                occlusionStatus = occlusion.analyse(evidence, occlusionOptions,
                        occlusionResult);
            if (occlusionStatus != NO_ERROR || occlusionResult.applied != 0)
                nativeAlgorithmTrace(jpeg_settings, "Occlusion", occlusionStatus,
                        jpeg_settings->frame_number, 1, hal_obj, occlusionResult.applied);
            // Real occlusion decision of this frame, published through the
            // diagnostics.occlusion* result keys. Only a result the stage really
            // produced is recorded (status 0); a refused or unavailable analysis
            // records nothing, and the result path publishes only what the frame
            // it refers to really carries.
            if (occlusionStatus == NO_ERROR)
                hal_obj->noteOcclusionResult(jpeg_settings->frame_number,
                        occlusionResult.occluded ? 1 : 0, occlusionResult.confidence,
                        occlusionResult.reason, occlusionResult.coreStatus);
        }
    }

    // clean and invalidate cache ops through mem obj of the frame
    memObj->cleanInvalidateCache(main_frame->buf_idx);

    if (mJpegClientHandle <= 0) {
        LOGE("Error: bug here, mJpegClientHandle is 0");
        return UNKNOWN_ERROR;
    }
    cam_dimension_t src_dim;
    memset(&src_dim, 0, sizeof(cam_dimension_t));
    main_stream->getFrameDimension(src_dim);

    cam_dimension_t dst_dim;
    memset(&dst_dim, 0, sizeof(cam_dimension_t));

    //For depth images calculate src and des dim from getDepthMapSize.
    if(jpeg_settings->image_type == CAM_HAL3_JPEG_TYPE_DEPTH)
    {
#ifdef ENABLE_QC_BOKEH
        qrcp::getDepthMapSize(src_dim.width, src_dim.height,
                            src_dim.width, src_dim.height);
#endif //ENABLE_QC_BOKEH
        dst_dim = src_dim;
    } else if(jpeg_settings->is_dim_valid){
        dst_dim = jpeg_settings->output_dim;
    } else {
        if (NO_ERROR != m_parent->getStreamSize(dst_dim)) {
            LOGE("Failed to get size of the JPEG stream");
            return UNKNOWN_ERROR;
        }
    }

    needJpegExifRotation = hal_obj->needJpegExifRotation();
    IF_META_AVAILABLE(cam_rotation_info_t, rotation_info, CAM_INTF_PARM_ROTATION, metadata) {
        if (jpeg_settings->jpeg_orientation != 0 && rotation_info->rotation == ROTATE_0) {
            needJpegExifRotation = TRUE;
            LOGH("Need EXIF JPEG ROTATION");
        }
    }

    // Although in HAL3, legacy flip mode is not advertised
    // default value of CAM_INTF_PARM_FLIP is still added here
    // for jpge metadata
    int32_t flipMode = 0; // no flip
    ADD_SET_PARAM_ENTRY_TO_BATCH(metadata, CAM_INTF_PARM_FLIP, flipMode);

    LOGH("Need new session?:%d jpeg_orientation %d needJpegExifRotation %d useExifRotation %d",
            needNewSess, jpeg_settings->jpeg_orientation, needJpegExifRotation,
            hal_obj->useExifRotation());
    if (needNewSess) {
        //creating a new session, so we must destroy the old one
        if ( 0 < mJpegSessionId ) {
            ret = mJpegHandle.destroy_session(mJpegSessionId);
            if (ret != NO_ERROR) {
                LOGE("Error destroying an old jpeg encoding session, id = %d",
                       mJpegSessionId);
                return ret;
            }
            mJpegSessionId = 0;
        }
        // create jpeg encoding session
        mm_jpeg_encode_params_t encodeParam;
        memset(&encodeParam, 0, sizeof(mm_jpeg_encode_params_t));
        if(jpeg_settings->image_type == CAM_HAL3_JPEG_TYPE_DEPTH ||
             jpeg_settings->image_type == CAM_HAL3_JPEG_TYPE_BOKEH)
        {
            getJpegEncodeConfig(encodeParam, main_stream, jpeg_settings, recvd_frame->bufs[0]);
        } else {
            getJpegEncodeConfig(encodeParam, main_stream, jpeg_settings);
        }
        LOGH("#src bufs:%d # tmb bufs:%d #dst_bufs:%d",
                     encodeParam.num_src_bufs,encodeParam.num_tmb_bufs,encodeParam.num_dst_bufs);
        if (!needJpegExifRotation &&
            (jpeg_settings->jpeg_orientation == 90 ||
            jpeg_settings->jpeg_orientation == 270)) {
           //swap src width and height, stride and scanline due to rotation
           encodeParam.main_dim.src_dim.width = src_dim.height;
           encodeParam.main_dim.src_dim.height = src_dim.width;
           encodeParam.thumb_dim.src_dim.width = src_dim.height;
           encodeParam.thumb_dim.src_dim.height = src_dim.width;

           int32_t temp = encodeParam.src_main_buf[0].offset.mp[0].stride;
           encodeParam.src_main_buf[0].offset.mp[0].stride =
              encodeParam.src_main_buf[0].offset.mp[0].scanline;
           encodeParam.src_main_buf[0].offset.mp[0].scanline = temp;

           temp = encodeParam.src_thumb_buf[0].offset.mp[0].stride;
           encodeParam.src_thumb_buf[0].offset.mp[0].stride =
              encodeParam.src_thumb_buf[0].offset.mp[0].scanline;
           encodeParam.src_thumb_buf[0].offset.mp[0].scanline = temp;
        } else {
           encodeParam.main_dim.src_dim  = src_dim;
           encodeParam.thumb_dim.src_dim = src_dim;
        }
        encodeParam.main_dim.dst_dim = dst_dim;
        encodeParam.thumb_dim.dst_dim = jpeg_settings->thumbnail_size;

        if (!hal_obj->useExifRotation() && needJpegExifRotation) {
            encodeParam.rotation = jpeg_settings->jpeg_orientation;
            encodeParam.thumb_rotation = jpeg_settings->jpeg_orientation;
        }

        LOGI("Src Buffer cnt = %d, res = %dX%d len = %d rot = %d "
            "src_dim = %dX%d dst_dim = %dX%d",
            encodeParam.num_src_bufs,
            encodeParam.src_main_buf[0].offset.mp[0].stride,
            encodeParam.src_main_buf[0].offset.mp[0].scanline,
            encodeParam.src_main_buf[0].offset.frame_len,
            encodeParam.rotation,
            src_dim.width, src_dim.height,
            dst_dim.width, dst_dim.height);
        LOGI("Src THUMB buf_cnt = %d, res = %dX%d len = %d rot = %d "
            "src_dim = %dX%d, dst_dim = %dX%d",
            encodeParam.num_tmb_bufs,
            encodeParam.src_thumb_buf[0].offset.mp[0].stride,
            encodeParam.src_thumb_buf[0].offset.mp[0].scanline,
            encodeParam.src_thumb_buf[0].offset.frame_len,
            encodeParam.thumb_rotation,
            encodeParam.thumb_dim.src_dim.width,
            encodeParam.thumb_dim.src_dim.height,
            encodeParam.thumb_dim.dst_dim.width,
            encodeParam.thumb_dim.dst_dim.height);
        ret = mJpegHandle.create_session(mJpegClientHandle, &encodeParam, &mJpegSessionId);
        if (ret != NO_ERROR) {
            LOGE("Error creating a new jpeg encoding session, ret = %d", ret);
            return ret;
        }
        needNewSess = FALSE;
    }

    // Fill in new job
    memset(&jpg_job, 0, sizeof(mm_jpeg_job_t));
    jpg_job.job_type = JPEG_JOB_TYPE_ENCODE;
    jpg_job.encode_job.session_id = mJpegSessionId;
    jpg_job.encode_job.src_index = (int32_t)main_frame->buf_idx;
    jpg_job.encode_job.dst_index = 0;

    cam_rect_t crop;
    memset(&crop, 0, sizeof(cam_rect_t));
    //TBD_later - Zoom event removed in stream
    //main_stream->getCropInfo(crop);
    if(jpeg_settings->is_crop_valid)
    {
        crop = jpeg_settings->crop;
    }else {
        crop.left = 0;
        crop.top = 0;
        crop.height = src_dim.height;
        crop.width = src_dim.width;
    }

    if (jpeg_settings->hdr_snapshot) {
       memset(&param, 0, sizeof(cam_stream_parm_buffer_t));
       param.type = CAM_STREAM_PARAM_TYPE_GET_OUTPUT_CROP;
       ret = main_stream->getParameter(param);
       if (ret != NO_ERROR) {
          LOGE("%s: stream getParameter for reprocess failed", __func__);
       } else {
           for (int i = 0; i < param.outputCrop.num_of_streams; i++) {
              if (param.outputCrop.crop_info[i].stream_id
                  == main_stream->getMyServerID()) {
                     crop = param.outputCrop.crop_info[i].crop;
                     main_stream->setCropInfo(crop);
              }
           }
         }
    }

    if (jpeg_settings->multiframe_snapshot) {
        memset(&param, 0, sizeof(cam_stream_parm_buffer_t));
        param.type = CAM_STREAM_PARAM_TYPE_GET_IMG_PROP;
        ret = main_stream->getParameter(param);
        if (ret != NO_ERROR) {
           LOGE(" stream getParameter for reprocess failed");
        } else {
            main_stream->setCropInfo(param.imgProp.crop);
            crop = param.imgProp.crop;
        }
    }

    // Set main dim job parameters and handle rotation
    if (!needJpegExifRotation && (jpeg_settings->jpeg_orientation == 90 ||
            jpeg_settings->jpeg_orientation == 270)) {

        jpg_job.encode_job.main_dim.src_dim.width = src_dim.height;
        jpg_job.encode_job.main_dim.src_dim.height = src_dim.width;

        jpg_job.encode_job.main_dim.dst_dim.width = dst_dim.height;
        jpg_job.encode_job.main_dim.dst_dim.height = dst_dim.width;

        jpg_job.encode_job.main_dim.crop.width = crop.height;
        jpg_job.encode_job.main_dim.crop.height = crop.width;
        jpg_job.encode_job.main_dim.crop.left = crop.top;
        jpg_job.encode_job.main_dim.crop.top = crop.left;
    } else {
        jpg_job.encode_job.main_dim.src_dim = src_dim;
        jpg_job.encode_job.main_dim.dst_dim = dst_dim;
        jpg_job.encode_job.main_dim.crop = crop;
    }

    // get 3a sw version info
    cam_q3a_version_t sw_version;
    memset(&sw_version, 0, sizeof(sw_version));

    if (hal_obj)
        hal_obj->get3AVersion(sw_version);

    if(jpeg_job_data->src_reproc_frame != NULL)
    {
        if((m_parent->getMyHandle() == jpeg_job_data->src_reproc_frame->ch_id)
                && (jpeg_job_data->src_reproc_frame->num_bufs != 0))
        {
            mm_camera_buf_def_t *buf = jpeg_job_data->src_reproc_frame->bufs[0];
            if(buf->mem_info && (buf->stream_type == CAM_STREAM_TYPE_SNAPSHOT))
            {
                QCamera3StreamMem *memObj =
                        (QCamera3StreamMem *)buf->mem_info;
                int bufidx = buf->buf_idx;
                jpg_job.encode_job.work_buf.buf_size = memObj->getSize(bufidx);
                jpg_job.encode_job.work_buf.buf_vaddr = (uint8_t *)memObj->getPtr(bufidx);
                jpg_job.encode_job.work_buf.fd = memObj->getFd(bufidx);
                memObj->invalidateCache(bufidx);
            }
        }
    }

    // get exif data
    QCamera3Exif *pJpegExifObj = getExifData(metadata, jpeg_settings,
            (needJpegExifRotation && hal_obj->useExifRotation()));
    jpeg_job_data->pJpegExifObj = pJpegExifObj;
    if (pJpegExifObj != NULL) {
        jpg_job.encode_job.exif_info.exif_data = pJpegExifObj->getEntries();
        jpg_job.encode_job.exif_info.numOfEntries =
            pJpegExifObj->getNumOfEntries();
        jpg_job.encode_job.exif_info.debug_data.sw_3a_version[0] =
            sw_version.major_version;
        jpg_job.encode_job.exif_info.debug_data.sw_3a_version[1] =
            sw_version.minor_version;
        jpg_job.encode_job.exif_info.debug_data.sw_3a_version[2] =
            sw_version.patch_version;
        jpg_job.encode_job.exif_info.debug_data.sw_3a_version[3] =
            sw_version.new_feature_des;
    }

    if (!hal_obj->useExifRotation() && needJpegExifRotation) {
        jpg_job.encode_job.rotation= jpeg_settings->jpeg_orientation;
    }

    // thumbnail dim
    LOGH("Thumbnail needed:%d", m_bThumbnailNeeded);
    if (m_bThumbnailNeeded == TRUE) {
        jpg_job.encode_job.thumb_dim.dst_dim =
                jpeg_settings->thumbnail_size;

      if (!needJpegExifRotation &&
          (jpeg_settings->jpeg_orientation  == 90 ||
           jpeg_settings->jpeg_orientation == 270)) {
            //swap the thumbnail destination width and height if it has
            //already been rotated
            int temp = jpg_job.encode_job.thumb_dim.dst_dim.width;
            jpg_job.encode_job.thumb_dim.dst_dim.width =
                    jpg_job.encode_job.thumb_dim.dst_dim.height;
            jpg_job.encode_job.thumb_dim.dst_dim.height = temp;

            jpg_job.encode_job.thumb_dim.src_dim.width = src_dim.height;
            jpg_job.encode_job.thumb_dim.src_dim.height = src_dim.width;

            jpg_job.encode_job.thumb_dim.crop.width = crop.height;
            jpg_job.encode_job.thumb_dim.crop.height = crop.width;
            jpg_job.encode_job.thumb_dim.crop.left = crop.top;
            jpg_job.encode_job.thumb_dim.crop.top = crop.left;
        } else {
           jpg_job.encode_job.thumb_dim.src_dim = src_dim;
           jpg_job.encode_job.thumb_dim.crop = crop;
        }
        jpg_job.encode_job.thumb_index = main_frame->buf_idx;
        LOGI("Thumbnail idx = %d src w/h (%dx%d), dst w/h (%dx%d)",
                jpg_job.encode_job.thumb_index,
                jpg_job.encode_job.thumb_dim.src_dim.width,
                jpg_job.encode_job.thumb_dim.src_dim.height,
                jpg_job.encode_job.thumb_dim.dst_dim.width,
                jpg_job.encode_job.thumb_dim.dst_dim.height);
    }
    LOGI("Main image idx = %d src w/h (%dx%d), dst w/h (%dx%d) rot = %d"
            "crop t/lt (%dx%d) wxh (%dx%d)",
            jpg_job.encode_job.src_index,
            jpg_job.encode_job.main_dim.src_dim.width,
            jpg_job.encode_job.main_dim.src_dim.height,
            jpg_job.encode_job.main_dim.dst_dim.width,
            jpg_job.encode_job.main_dim.dst_dim.height,
            jpg_job.encode_job.rotation,
            jpg_job.encode_job.main_dim.crop.top,
            jpg_job.encode_job.main_dim.crop.left,
            jpg_job.encode_job.main_dim.crop.width,
            jpg_job.encode_job.main_dim.crop.height);

    jpg_job.encode_job.cam_exif_params = hal_obj->get3AExifParams();
    exif_debug_params = jpg_job.encode_job.cam_exif_params.debug_params;
    jpg_job.encode_job.cam_exif_params.debug_params = NULL;

    jpg_job.encode_job.mobicat_mask = hal_obj->getMobicatMask();

    if (metadata != NULL) {
       //Fill in the metadata passed as parameter
        jpg_job.encode_job.p_metadata = metadata;
       if(!metadata->is_valid[CAM_INTF_META_AEC_INFO]) {
            if(jpg_job.encode_job.cam_exif_params.cam_3a_params_valid) {
                jpg_job.encode_job.p_metadata->is_valid[CAM_INTF_META_AEC_INFO] =
                    jpg_job.encode_job.cam_exif_params.cam_3a_params_valid;
                cam_3a_params_t *ptr_mobicat_aec_params =
                    POINTER_OF_META(CAM_INTF_META_AEC_INFO, jpg_job.encode_job.p_metadata);
                *ptr_mobicat_aec_params = jpg_job.encode_job.cam_exif_params.cam_3a_params;
            }
        }

        if (exif_debug_params) {
            if(!(IS_META_VALID(metadata,CAM_INTF_META_EXIF_DEBUG_AE)))
            {
                if(exif_debug_params->ae_debug_params_valid) {
                    jpg_job.encode_job.p_metadata->is_valid[CAM_INTF_META_EXIF_DEBUG_AE] =
                        exif_debug_params->ae_debug_params_valid;
                    cam_ae_exif_debug_t *ptr_statsdebug_ae_data =
                        POINTER_OF_META(CAM_INTF_META_EXIF_DEBUG_AE, jpg_job.encode_job.p_metadata);
                    *ptr_statsdebug_ae_data =
                        exif_debug_params->ae_debug_params;
                }
            }

            if(!(IS_META_VALID(metadata, CAM_INTF_META_EXIF_DEBUG_AWB)))
            {
                if(exif_debug_params->awb_debug_params_valid) {
                    jpg_job.encode_job.p_metadata->is_valid[CAM_INTF_META_EXIF_DEBUG_AWB] =
                        exif_debug_params->awb_debug_params_valid;
                    cam_awb_exif_debug_t *ptr_statsdebug_awb_data =
                       POINTER_OF_META(CAM_INTF_META_EXIF_DEBUG_AWB, jpg_job.encode_job.p_metadata);
                    *ptr_statsdebug_awb_data =
                        exif_debug_params->awb_debug_params;
                }
            }

            if(!(IS_META_VALID(metadata, CAM_INTF_META_EXIF_DEBUG_AF)))
            {
                if(exif_debug_params->af_debug_params_valid) {
                    jpg_job.encode_job.p_metadata->is_valid[CAM_INTF_META_EXIF_DEBUG_AF] =
                        exif_debug_params->af_debug_params_valid;
                    cam_af_exif_debug_t *ptr_statsdebug_af_data =
                        POINTER_OF_META(CAM_INTF_META_EXIF_DEBUG_AF, jpg_job.encode_job.p_metadata);
                    *ptr_statsdebug_af_data =
                        exif_debug_params->af_debug_params;
                }
            }

            if(!(IS_META_VALID(metadata, CAM_INTF_META_EXIF_DEBUG_ASD)))
            {
                if(exif_debug_params->asd_debug_params_valid) {
                    jpg_job.encode_job.p_metadata->is_valid[CAM_INTF_META_EXIF_DEBUG_ASD] =
                        exif_debug_params->asd_debug_params_valid;
                    cam_asd_exif_debug_t *ptr_statsdebug_asd_data =
                       POINTER_OF_META(CAM_INTF_META_EXIF_DEBUG_ASD, jpg_job.encode_job.p_metadata);
                    *ptr_statsdebug_asd_data =
                        exif_debug_params->asd_debug_params;
                }
            }

            if(!(IS_META_VALID(metadata, CAM_INTF_META_EXIF_DEBUG_STATS)))
            {
                if(exif_debug_params->stats_debug_params_valid) {
                    jpg_job.encode_job.p_metadata->is_valid[CAM_INTF_META_EXIF_DEBUG_STATS] =
                        exif_debug_params->stats_debug_params_valid;
                    cam_stats_buffer_exif_debug_t *ptr_statsdebug_stats_buffer_data =
                     POINTER_OF_META(CAM_INTF_META_EXIF_DEBUG_STATS, jpg_job.encode_job.p_metadata);
                    *ptr_statsdebug_stats_buffer_data =
                        exif_debug_params->stats_debug_params;
                }
            }

            if(!(IS_META_VALID(metadata, CAM_INTF_META_EXIF_DEBUG_BESTATS)))
            {
                if(exif_debug_params->bestats_debug_params_valid) {
                    jpg_job.encode_job.p_metadata->is_valid[CAM_INTF_META_EXIF_DEBUG_BESTATS] =
                        exif_debug_params->bestats_debug_params_valid;
                    cam_bestats_buffer_exif_debug_t *ptr_statsdebug_bestats_buffer_data =
                        POINTER_OF_META(CAM_INTF_META_EXIF_DEBUG_BESTATS,
                            jpg_job.encode_job.p_metadata);
                    *ptr_statsdebug_bestats_buffer_data =
                        exif_debug_params->bestats_debug_params;
                }
            }

            if(!(IS_META_VALID(metadata, CAM_INTF_META_EXIF_DEBUG_BHIST)))
            {
                if(exif_debug_params->bhist_debug_params_valid) {
                    jpg_job.encode_job.p_metadata->is_valid[CAM_INTF_META_EXIF_DEBUG_BHIST] =
                        exif_debug_params->bhist_debug_params_valid;
                    cam_bhist_buffer_exif_debug_t *ptr_statsdebug_bhist_data =
                    POINTER_OF_META(CAM_INTF_META_EXIF_DEBUG_BHIST, jpg_job.encode_job.p_metadata);
                    *ptr_statsdebug_bhist_data =
                        exif_debug_params->bhist_debug_params;
                }
            }

            if(!(IS_META_VALID(metadata, CAM_INTF_META_EXIF_DEBUG_3A_TUNING)))
            {
                if(exif_debug_params->q3a_tuning_debug_params_valid) {
                    jpg_job.encode_job.p_metadata->is_valid[CAM_INTF_META_EXIF_DEBUG_3A_TUNING] =
                        exif_debug_params->q3a_tuning_debug_params_valid;
                    cam_q3a_tuning_info_t *ptr_statsdebug_3a_tuning_data =
                        POINTER_OF_META(CAM_INTF_META_EXIF_DEBUG_3A_TUNING,
                            jpg_job.encode_job.p_metadata);
                    *ptr_statsdebug_3a_tuning_data =
                        exif_debug_params->q3a_tuning_debug_params;
                }
            }
        }
    } else {
       LOGW("Metadata is null");
    }

    // Multi image info
    if ((hal_obj->isDeviceLinked() == TRUE) || (jpeg_settings->encode_type == MM_JPEG_TYPE_MPO )) {
        jpg_job.encode_job.multi_image_info.type = jpeg_settings->encode_type;
        jpg_job.encode_job.multi_image_info.num_of_images = 
                                        (jpeg_settings->encode_type == MM_JPEG_TYPE_MPO)? 3 : 1;
        jpg_job.encode_job.multi_image_info.enable_metadata = 0;
        if (hal_obj->isMainCamera() == TRUE && (jpeg_settings->encode_type == MM_JPEG_TYPE_MPO ?
                    jpeg_settings->image_type == CAM_HAL3_JPEG_TYPE_BOKEH : false)) {
            jpg_job.encode_job.multi_image_info.is_primary = 1;
        } else {
            jpg_job.encode_job.multi_image_info.is_primary = 0;
        }
    }

    jpg_job.encode_job.hal_version = CAM_HAL_V3;

    //Start jpeg encoding
    ret = mJpegHandle.start_job(&jpg_job, &jobId);
    if (jpg_job.encode_job.cam_exif_params.debug_params) {
        free(jpg_job.encode_job.cam_exif_params.debug_params);
    }
    if (ret == NO_ERROR) {
        // remember job info
        jpeg_job_data->jobId = jobId;
    }

    LOGD("X");
    return ret;
}

/*===========================================================================
 * FUNCTION   : doNextJob
 *
 * DESCRIPTION: send DO_NEXT_JOB command to dataProc thread.
 *
 * PARAMETERS : none
 *
 * RETURN     : none
 *==========================================================================*/
 void QCamera3PostProcessor::doNextJob()
{
    m_dataProcTh.sendCmd(CAMERA_CMD_TYPE_DO_NEXT_JOB, FALSE, FALSE);
}

/*===========================================================================
 * FUNCTION   : composeMpo
 *
 * DESCRIPTION: compress jpeg images to mpo format. Use jpeg interface handle
 *              to compose mpo image. Sends callback to pic channel (mpoevthandle)
 *              with composed mpo.
 *
 * PARAMETERS :
 *   @mpo_info : contain list of input images and output buffer.
 *   @userdata : need to pass with mpo callback.
 *
 * RETURN     : NO_ERROR on success else -1.
 *==========================================================================*/
int32_t QCamera3PostProcessor::composeMpo(qcamera_hal3_mpo_compose_info_t &mpo_info,
                                                                         void *userdata)
{
    LOGH("E");
    if((mpo_info.num_of_aux_image + 1) > MM_JPEG_MAX_MPO_IMAGES)
    {
        LOGE("Error: cannot composeMpo of %d images",mpo_info.num_of_aux_image + 1);
        return -1;
    }

    ssize_t output_size = 0;
    mm_jpeg_mpo_info_t mpo_compose_info;
    mpo_compose_info.primary_image = mpo_info.main_image;   //first images will be the primary image
    output_size += mpo_info.main_image.buf_filled_len;

    for(uint32_t i = 0; i < mpo_info.num_of_aux_image; i++)
    {
        mpo_compose_info.aux_images[i] = mpo_info.aux_images[i];
        output_size += mpo_info.aux_images[i].buf_filled_len;
    }
    mpo_compose_info.num_of_images = mpo_info.num_of_aux_image + 1 /*num of main image*/;
    mpo_compose_info.output_buff = mpo_info.output;
    mpo_compose_info.output_buff.buf_filled_len = 0;
    mpo_compose_info.output_buff_size = output_size;

    int rc = mMpoHandle.compose_mpo(&mpo_compose_info);
    if(rc != 0)
    {
        LOGE("Error: failed to compose mpo image");
        return -1;
    }

    mMpoCB( JPEG_JOB_STATUS_DONE, &mpo_compose_info.output_buff, userdata);

    LOGH("X");
    return NO_ERROR;
}

/*===========================================================================
 * FUNCTION   : clearPostprocQueues
 *
 * DESCRIPTION: release queued owners while their channels and buffers live.
 *==========================================================================*/
void QCamera3PostProcessor::clearPostprocQueues()
{
    // cancel all ongoing jpeg jobs
    qcamera_hal3_jpeg_data_t *jpeg_job =
        (qcamera_hal3_jpeg_data_t *)m_ongoingJpegQ.dequeue();
    while (jpeg_job != NULL) {
        mJpegHandle.abort_job(jpeg_job->jobId);

        releaseJpegData(jpeg_job, this);
        free(jpeg_job);
        jpeg_job = (qcamera_hal3_jpeg_data_t *)m_ongoingJpegQ.dequeue();
    }

    m_ongoingJpegQ.flushOutsideLock();

    // destroy jpeg encoding session
    if ( 0 < mJpegSessionId ) {
        mJpegHandle.destroy_session(mJpegSessionId);
        mJpegSessionId = 0;
    }

    releaseFreeJpegSessions();

    // flush ongoing postproc Queue
    m_ongoingPPQ.flushOutsideLock();

    m_inputRawQ.flushOutsideLock();

    // flush input jpeg Queue
    m_inputJpegQ.flushOutsideLock();

    // flush framework input Postproc Queue
    m_inputFWKPPQ.flush();

    m_inputMultiReprocQ.flushOutsideLock();

    // Matching callbacks also use mReprocJobLock. Close both
    // unmatched queues and detach matched pairs as one operation.
    List<ReprocessBuffer> abandoned;
    pthread_mutex_lock(&mReprocJobLock);
    m_inputPPQ.flushOutsideLock();
    m_inputMetaQ.flushOutsideLock();
    m_jpegSettingsQ.flush();
    while (mReprocessNode.size()) {
        abandoned.push_back(*mReprocessNode.begin());
        mReprocessNode.erase(mReprocessNode.begin());
    }
    pthread_mutex_unlock(&mReprocJobLock);
    while (abandoned.size())
    {
        List<ReprocessBuffer>::iterator reprocData = abandoned.begin();
        qcamera_hal3_pp_buffer_t *pp_buffer = reprocData->reprocBuf;
        qcamera_hal3_meta_pp_buffer_t *meta_pp_buffer = reprocData->metaBuffer;

        abandoned.erase(reprocData);
        // free frame
        if (pp_buffer != NULL) {
            releasePPInputData(pp_buffer, this);
            free(pp_buffer);
        }
        //free metadata
        if (NULL != meta_pp_buffer) {
            releaseMetadata(meta_pp_buffer, this);
            free(meta_pp_buffer);
        }
    }
}

void *QCamera3PostProcessor::dataProcessRoutine(void *data)
{
    int running = 1;
    int ret;
    uint8_t is_active = FALSE;
    uint8_t needNewSess = TRUE;
    uint8_t sync = FALSE;
    mm_camera_super_buf_t *meta_buffer = NULL;
    LOGD("E");
    QCamera3PostProcessor *pme = (QCamera3PostProcessor *)data;
    QCameraCmdThread *cmdThread = &pme->m_dataProcTh;
    cmdThread->setName("cam_data_proc");

    do {
        do {
            ret = cam_sem_wait(&cmdThread->cmd_sem);
            if (ret != 0 && errno != EINVAL) {
                LOGE("cam_sem_wait error (%s)",
                            strerror(errno));
                return NULL;
            }
        } while (ret != 0);

        sync = FALSE;
        // we got notified about new cmd avail in cmd queue
        camera_cmd_type_t cmd = cmdThread->getCmd(sync);
        switch (cmd) {
        case CAMERA_CMD_TYPE_START_DATA_PROC:
            LOGH("start data proc");
            is_active = TRUE;
            needNewSess = TRUE;

            pme->m_ongoingPPQ.init();
            pme->m_ongoingJpegQ.init();
            pme->m_inputJpegQ.init();
            pme->m_inputPPQ.init();
            pme->m_inputFWKPPQ.init();
            pme->m_inputMultiReprocQ.init();
            pme->m_inputMetaQ.init();
            if(sync)
            {
                cam_sem_post(&cmdThread->sync_sem);
            }

            break;
        case CAMERA_CMD_TYPE_CANCEL_PP_FRAME:
            {
                pme->cancelQueuedFrames();
            }
            break;
        case CAMERA_CMD_TYPE_PAUSE_DATA_PROC:
            is_active = FALSE;
            if (sync) cam_sem_post(&cmdThread->sync_sem);
            break;
        case CAMERA_CMD_TYPE_STOP_DATA_PROC:
            {
                LOGH("stop data proc");
                pme->abortHdrCapture();
                is_active = FALSE;
                pme->stopPostproc();
                needNewSess = TRUE;
                // signal cmd is completed
                if(sync)
                {
                    cam_sem_post(&cmdThread->sync_sem);
                }
            }
            break;
        case CAMERA_CMD_TYPE_DO_NEXT_JOB:
            {
                LOGH("Do next job, active is %d", is_active);
                /* needNewSess is set to TRUE as postproc is not re-STARTed
                 * anymore for every captureRequest */
                needNewSess = TRUE;
                if (is_active == TRUE) {
                    // Cancellation may have been recorded after an earlier
                    // DO_NEXT_JOB was queued. Honor it before submitting work.
                    pme->cancelQueuedFrames();
                    pme->processDualCapture();
                    // check if there is any ongoing jpeg jobs
                    if (pme->m_ongoingJpegQ.isEmpty()) {
                        LOGD("ongoing jpeg queue is empty so doing the jpeg job");
                        // no ongoing jpeg job, we are fine to send jpeg encoding job
                        qcamera_hal3_jpeg_data_t *jpeg_job =
                            (qcamera_hal3_jpeg_data_t *)pme->m_inputJpegQ.dequeue();

                        if (NULL != jpeg_job) {
                            // add into ongoing jpeg job Q
                            pme->m_ongoingJpegQ.enqueue((void *)jpeg_job);

                            if (jpeg_job->fwk_frame) {
                                ret = pme->encodeFWKData(jpeg_job, needNewSess);
                            } else {
                                ret = pme->encodeData(jpeg_job, needNewSess);
                            }
                            if (NO_ERROR != ret) {
                                // dequeue the last one
                                pme->m_ongoingJpegQ.dequeue(false);

                                pme->releaseJpegData(jpeg_job, pme);
                                free(jpeg_job);
                            }
                            pme->releaseFreeJpegSessions();
                        }
                    }

                    if (!pme->m_inputMultiReprocQ.isEmpty()) {
                        QCamera3HardwareInterface* hal_obj =
                            (QCamera3HardwareInterface *)pme->m_parent->mUserData;
                        qcamera_hal3_pp_data_t *pp_job =
                            (qcamera_hal3_pp_data_t *)pme->m_inputMultiReprocQ.dequeue();
                        if (pp_job != NULL) {
                            LOGH("multi reproc Q is not empty, pp channel idx:%d, total pp cnt:%d",
                                pp_job->pp_ch_idx, pme->m_ppChannelCnt);
                            if (pp_job->pp_ch_idx < pme->m_ppChannelCnt &&
                                pme->m_pReprocChannel[pp_job->pp_ch_idx] != NULL) {
                                LOGH("do reproc on %dth reprocess channel", pp_job->pp_ch_idx + 1);

                                qcamera_fwk_input_pp_data_t fwk_frame;
                                memset(&fwk_frame, 0, sizeof(qcamera_fwk_input_pp_data_t));
                                if (pp_job->fwk_src_frame != NULL) {
                                    LOGD("reprocess for fwk input frame");
                                    fwk_frame = *(pp_job->fwk_src_frame);
                                    fwk_frame.input_buffer
                                        = *(pp_job->reprocessed_src_frame->bufs[0]);
                                } else {
                                    fwk_frame.frameNumber = pp_job->frameNumber;
                                    fwk_frame.input_buffer
                                        = *(pp_job->reprocessed_src_frame->bufs[0]);
                                    fwk_frame.metadata_buffer = *(pp_job->src_metadata->bufs[0]);

                                    uint32_t stream_id = hal_obj->mQCFACaptureChannel->getStreamSvrId();
                                    LOGD("src stream server id:%d", stream_id);
                                    ret = pme->m_pReprocChannel[pp_job->pp_ch_idx]->overrideMetadata(
                                            (metadata_buffer_t *)fwk_frame.metadata_buffer.buffer,
                                            pp_job->jpeg_settings, stream_id);
                                    if (ret != NO_ERROR) {
                                        LOGE("fail to override metadata.");
                                    }
                                }
                                LOGD("frame number: %d", fwk_frame.frameNumber);
                                pme->m_ongoingPPQ.enqueue((void *)pp_job);
                                ret = pme->m_pReprocChannel[pp_job->pp_ch_idx]->doReprocessOffline(
                                    &fwk_frame, true);
                                if (ret != NO_ERROR) {
                                    pme->m_ongoingPPQ.dequeue(false);
                                    LOGE("fail to do offline reprocess");
                                }
                            }
                        } else {
                            LOGE("fail to dequeue pp job!");
                        }
                    }

                    // check if there are any framework pp jobs
                    if (!pme->m_inputFWKPPQ.isEmpty()) {
                        qcamera_fwk_input_pp_data_t *fwk_frame =
                                (qcamera_fwk_input_pp_data_t *) pme->m_inputFWKPPQ.dequeue();
                        if (NULL != fwk_frame) {
                            qcamera_hal3_pp_data_t *pp_job =
                                (qcamera_hal3_pp_data_t *)malloc(sizeof(qcamera_hal3_pp_data_t));
                            jpeg_settings_t *jpeg_settings =
                                (jpeg_settings_t *)pme->m_jpegSettingsQ.dequeue();
                            if (pp_job != NULL) {
                                memset(pp_job, 0, sizeof(qcamera_hal3_pp_data_t));
                                pp_job->jpeg_settings = jpeg_settings;
                                if (pme->m_pReprocChannel[0] != NULL) {
                                    if (NO_ERROR !=
                                        pme->m_pReprocChannel[0]->overrideFwkMetadata(fwk_frame)) {
                                        LOGE("Failed to extract output crop");
                                    }
                                    // add into ongoing PP job Q
                                    pp_job->fwk_src_frame = fwk_frame;
                                    pp_job->pp_ch_idx = 0;
                                    pp_job->frameNumber = fwk_frame->frameNumber;
                                    pme->m_ongoingPPQ.enqueue((void *)pp_job);
                                    ret = pme->m_pReprocChannel[0]->doReprocessOffline(fwk_frame);
                                    if (NO_ERROR != ret) {
                                        // remove from ongoing PP job Q
                                        pme->m_ongoingPPQ.dequeue(matchPPJob,
                                                &fwk_frame->frameNumber);
                                    }
                                } else {
                                    LOGE("Reprocess channel is NULL");
                                    ret = -1;
                                }
                            } else {
                                LOGE("no mem for qcamera_hal3_pp_data_t");
                                ret = -1;
                            }

                            if (0 != ret) {
                                // free pp_job
                                if (pp_job != NULL) {
                                    free(pp_job);
                                }
                                // free frame
                                if (fwk_frame != NULL) {
                                    free(fwk_frame);
                                }
                            }
                        }
                    }

                    LOGH("dequeuing pp frame");
                    pthread_mutex_lock(&pme->mReprocJobLock);
                    if(pme->mReprocessNode.size()) {
                        List<ReprocessBuffer>::iterator reprocData;
                        reprocData = pme->mReprocessNode.begin();
                        qcamera_hal3_pp_buffer_t *pp_buffer = reprocData->reprocBuf;
                        qcamera_hal3_meta_pp_buffer_t *meta_pp_buffer = reprocData->metaBuffer;
                        pme->mReprocessNode.erase(pme->mReprocessNode.begin());
                        LOGD(" Reprocess Buffer Frame Number :%d  and %d",
                                pp_buffer->frameNumber, meta_pp_buffer->metaFrameNumber);
                        meta_buffer =
                            (mm_camera_super_buf_t *)meta_pp_buffer->metabuf;
                        jpeg_settings_t *jpeg_settings =
                           (jpeg_settings_t *)pme->m_jpegSettingsQ.dequeue(
                                   matchJpegSetting, &pp_buffer->frameNumber);
                        jpeg_settings_t *ppOutput_jpeg_settings = NULL;

                        //In bokeh case, there will be no AUX image jpeg settings.
                        //DEPTH image jpeg_settings need to assign to ppOutPut_jpeg_settings.
                        //BOKEH image jpeg_settings need to assign to ppOutPut_jpeg_settings.
                        QCamera3HardwareInterface* hal_obj =
                                (QCamera3HardwareInterface*)pme->m_parent->mUserData;
                        if(hal_obj->isDualCamera() && jpeg_settings != NULL)
                        {
                            if(jpeg_settings->image_type != CAM_HAL3_JPEG_TYPE_MAIN)
                            {
                                ppOutput_jpeg_settings = jpeg_settings;
                                jpeg_settings = (jpeg_settings_t *)pme->m_jpegSettingsQ.dequeue();
                            } else {
                                QCamera3HardwareInterface *hal_obj =
                                        (QCamera3HardwareInterface *)pme->m_parent->mUserData;
                                if(hal_obj->getHalPPType() == CAM_HAL_PP_TYPE_BOKEH)
                                {
                                    ppOutput_jpeg_settings = (jpeg_settings_t *)
                                                                pme->m_jpegSettingsQ.dequeue();
                                }
                            }
                        }

                        pthread_mutex_unlock(&pme->mReprocJobLock);
                        qcamera_hal3_pp_data_t *pp_job =
                            (qcamera_hal3_pp_data_t *)malloc(sizeof(qcamera_hal3_pp_data_t));
                        if (pp_job == NULL) {
                            LOGE("no mem for qcamera_hal3_pp_data_t");
                            ret = -1;
                        } else if (meta_buffer == NULL) {
                            LOGE("failed to dequeue from m_inputMetaQ");
                            ret = -1;
                        } else if (pp_buffer == NULL) {
                            LOGE("failed to dequeue from m_inputPPQ");
                            ret = -1;
                        } else if (pp_buffer != NULL){
                            memset(pp_job, 0, sizeof(qcamera_hal3_pp_data_t));
                            pp_job->src_frame = pp_buffer->input;
                            pp_job->src_metadata = meta_buffer;
                            if (meta_buffer->bufs[0] != NULL) {
                                pp_job->metadata = (metadata_buffer_t *)
                                        meta_buffer->bufs[0]->buffer;
                            }
                            pp_job->jpeg_settings = jpeg_settings;
                            pp_job->ppOutput_jpeg_settings = ppOutput_jpeg_settings;

                            pp_job->pp_ch_idx = 0;
                            pp_job->frameNumber = pp_buffer->frameNumber;
                            pme->m_ongoingPPQ.enqueue((void *)pp_job);
                            if (pme->m_pReprocChannel[0] != NULL) {
                                mm_camera_buf_def_t *meta_buffer_arg = NULL;
                                meta_buffer_arg = meta_buffer->bufs[0];
                                qcamera_fwk_input_pp_data_t fwk_frame;
                                memset(&fwk_frame, 0, sizeof(qcamera_fwk_input_pp_data_t));
                                fwk_frame.frameNumber = pp_buffer->frameNumber;
                                if (pme->m_ppChannelCnt > 1) {
                                    LOGD("multi pass reprocess, no need override meta here.");
                                    fwk_frame.input_buffer = *(pp_buffer->input->bufs[0]);
                                    fwk_frame.metadata_buffer = *(meta_buffer->bufs[0]);
                                    fwk_frame.output_buffer = pp_buffer->output;
                                } else {
                                    if(pp_job->jpeg_settings != NULL)
                                    {
                                        if(hal_obj->m_bInSensorQCFA &&
                                                (hal_obj->mQCFACaptureChannel != NULL)) {
                                            uint32_t stream_id =
                                                hal_obj->mQCFACaptureChannel->getStreamSvrId();
                                            fwk_frame.input_buffer = *(pp_buffer->input->bufs[0]);
                                            fwk_frame.metadata_buffer = *(meta_buffer->bufs[0]);
                                            fwk_frame.output_buffer = pp_buffer->output;
                                            LOGD("src stream server id:%d", stream_id);
                                            ret = pme->m_pReprocChannel[0]->overrideMetadata(
                                                    (metadata_buffer_t *)meta_buffer_arg->buffer,
                                                    pp_job->jpeg_settings, stream_id);
                                        } else {
                                            ret = pme->m_pReprocChannel[0]->overrideMetadata(
                                                    pp_buffer, meta_buffer_arg,
                                                    pp_job->jpeg_settings,
                                                    fwk_frame);
                                        }
                                    } else {
                                        ret = pme->m_pReprocChannel[0]->overrideMetadata(
                                                pp_buffer, meta_buffer_arg,
                                                pp_job->ppOutput_jpeg_settings,
                                                fwk_frame);
                                    }
                                }
                                if (NO_ERROR == ret) {
                                    // add into ongoing PP job Q
                                    pme->mPerfLockMgr.acquirePerfLock(PERF_LOCK_OFFLINE_REPROC);
                                    ret = pme->m_pReprocChannel[0]->doReprocessOffline(
                                            &fwk_frame, true);
                                    pme->mPerfLockMgr.releasePerfLock(PERF_LOCK_OFFLINE_REPROC);
                                    if (NO_ERROR != ret) {
                                        // remove from ongoing PP job Q
                                        pme->m_ongoingPPQ.dequeue(matchPPJob,
                                                &pp_buffer->frameNumber);
                                    }
                                }
                            } else {
                                LOGE("No reprocess. Calling processPPData directly");
                                ret = pme->processPPData(pp_buffer->input);
                            }
                        }

                        if (0 != ret) {
                            if (pp_buffer && (pme->isHdrCaptureFrame(pp_buffer->frameNumber) ||
                                    pme->isDiscardedHdrFrame(pp_buffer->frameNumber))) {
                                LOGE("HAL3 HDR CPP submission failed for %u (%d)",
                                        pp_buffer->frameNumber, ret);
                                pme->cancelHdrCapture(pp_buffer->frameNumber);
                                pme->releaseOfflineBuffersForFrame(pp_buffer->frameNumber);
                                pme->m_ongoingPPQ.dequeue(matchPPJob, &pp_buffer->frameNumber);
                                if (pp_job && meta_buffer && pp_buffer->input) {
                                    pme->discardHdrFrame(pp_job, NULL);
                                    pp_job = NULL;
                                    pp_buffer->input = NULL;
                                    meta_buffer = NULL;
                                }
                            }
                            // free pp_job
                            if (pp_job != NULL) {
                                free(pp_job);
                            }
                            // free frame
                            if (pp_buffer != NULL) {
                                if (pp_buffer->input) {
                                    pme->releaseSuperBuf(pp_buffer->input);
                                    free(pp_buffer->input);
                                }
                                free(pp_buffer);
                            }
                            //free metadata
                            if (NULL != meta_buffer) {
                                pme->m_parent->metadataBufDone(meta_buffer);
                                free(meta_buffer);
                            }
                        } else {
                            if (pp_buffer != NULL) {
                                free(pp_buffer);
                            }
                        }
                        if (NULL != meta_pp_buffer) {
                            free(meta_pp_buffer);
                            meta_pp_buffer = NULL;
                        }
                    } else {
                        pthread_mutex_unlock(&pme->mReprocJobLock);
                    }
                } else {
                    // PAUSE preserves owners until driver callbacks stop.
                    // STOP/deinit drains all queues with their owner callbacks.
                }
            }
            break;
        case CAMERA_CMD_TYPE_INIT_JPEG:{
                if(pme->mJpegClientHandle <= 0)
                {
                    pme->mJpegClientHandle = jpeg_open(&pme->mJpegHandle, &pme->mMpoHandle,
                                               pme->max_pic_size, &pme->mJpegMetadata);
                    if (!pme->mJpegClientHandle) {
                        LOGE("jpeg_open did not work");
                    }
                }
            }
            break;
        case CAMERA_CMD_TYPE_EXIT:
            running = 0;
            break;
        default:
            break;
        }
    } while (running);
    LOGD("X");
    return NULL;
}

/* EXIF related helper methods */

/*===========================================================================
 * FUNCTION   : getRational
 *
 * DESCRIPTION: compose rational struct
 *
 * PARAMETERS :
 *   @rat     : ptr to struct to store rational info
 *   @num     :num of the rational
 *   @denom   : denom of the rational
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t getRational(rat_t *rat, int num, int denom)
{
    if ((0 > num) || (0 >= denom)) {
        LOGE("Negative values");
        return BAD_VALUE;
    }
    if (NULL == rat) {
        LOGE("NULL rat input");
        return BAD_VALUE;
    }
    rat->num = (uint32_t)num;
    rat->denom = (uint32_t)denom;
    return NO_ERROR;
}

/*===========================================================================
 * FUNCTION   : parseGPSCoordinate
 *
 * DESCRIPTION: parse GPS coordinate string
 *
 * PARAMETERS :
 *   @coord_str : [input] coordinate string
 *   @coord     : [output]  ptr to struct to store coordinate
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int parseGPSCoordinate(const char *coord_str, rat_t* coord)
{
    if(coord == NULL) {
        LOGE("error, invalid argument coord == NULL");
        return BAD_VALUE;
    }
    double degF = atof(coord_str);
    if (degF < 0) {
        degF = -degF;
    }
    double minF = (degF - (int) degF) * 60;
    double secF = (minF - (int) minF) * 60;

    getRational(&coord[0], (int)degF, 1);
    getRational(&coord[1], (int)minF, 1);
    getRational(&coord[2], (int)(secF * 10000), 10000);
    return NO_ERROR;
}

/*===========================================================================
 * FUNCTION   : getExifDateTime
 *
 * DESCRIPTION: query exif date time
 *
 * PARAMETERS :
 *   @dateTime   : string to store exif date time
 *   @subsecTime : string to store exif subsec time
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t getExifDateTime(String8 &dateTime, String8 &subsecTime)
{
    int32_t ret = NO_ERROR;

    //get time and date from system
    struct timeval tv;
    struct tm timeinfo_data;

    int res = gettimeofday(&tv, NULL);
    if (0 == res) {
        struct tm *timeinfo = localtime_r(&tv.tv_sec, &timeinfo_data);
        if (NULL != timeinfo) {
            //Write datetime according to EXIF Spec
            //"YYYY:MM:DD HH:MM:SS" (20 chars including \0)
            dateTime = String8::format("%04d:%02d:%02d %02d:%02d:%02d",
                    timeinfo->tm_year + 1900, timeinfo->tm_mon + 1,
                    timeinfo->tm_mday, timeinfo->tm_hour,
                    timeinfo->tm_min, timeinfo->tm_sec);
            //Write subsec according to EXIF Sepc
            subsecTime = String8::format("%06ld", tv.tv_usec);
        } else {
            LOGE("localtime_r() error");
            ret = UNKNOWN_ERROR;
        }
    } else if (-1 == res) {
        LOGE("gettimeofday() error: %s", strerror(errno));
        ret = UNKNOWN_ERROR;
    } else {
        LOGE("gettimeofday() unexpected return code: %d", res);
        ret = UNKNOWN_ERROR;
    }

    return ret;
}

/*===========================================================================
 * FUNCTION   : getExifFocalLength
 *
 * DESCRIPTION: get exif focal length
 *
 * PARAMETERS :
 *   @focalLength : ptr to rational struct to store focal length
 *   @value       : focal length value
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t getExifFocalLength(rat_t *focalLength, float value)
{
    int focalLengthValue =
        (int)(value * FOCAL_LENGTH_DECIMAL_PRECISION);
    return getRational(focalLength, focalLengthValue, FOCAL_LENGTH_DECIMAL_PRECISION);
}

/*===========================================================================
  * FUNCTION   : getExifExpTimeInfo
  *
  * DESCRIPTION: get exif exposure time information
  *
  * PARAMETERS :
  *   @expoTimeInfo     : rational exposure time value
  *   @value            : exposure time value
  * RETURN     : nt32_t type of status
  *              NO_ERROR  -- success
  *              none-zero failure code
  *==========================================================================*/
int32_t getExifExpTimeInfo(rat_t *expoTimeInfo, int64_t value)
{

    int64_t cal_exposureTime;
    if (value != 0)
        cal_exposureTime = value;
    else
        cal_exposureTime = 60;

    return getRational(expoTimeInfo, 1, (int)cal_exposureTime);
}

/*===========================================================================
 * FUNCTION   : getExifGpsProcessingMethod
 *
 * DESCRIPTION: get GPS processing method
 *
 * PARAMETERS :
 *   @gpsProcessingMethod : string to store GPS process method
 *   @count               : length of the string
 *   @value               : the value of the processing method
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t getExifGpsProcessingMethod(char *gpsProcessingMethod,
        uint32_t &count, char* value)
{
    if(value != NULL) {
        memcpy(gpsProcessingMethod, ExifAsciiPrefix, EXIF_ASCII_PREFIX_SIZE);
        count = EXIF_ASCII_PREFIX_SIZE;
        strlcpy(gpsProcessingMethod + EXIF_ASCII_PREFIX_SIZE,
                value,
                GPS_PROCESSING_METHOD_SIZE);
        count += (uint32_t)strlen(value);
        gpsProcessingMethod[count++] = '\0'; // increase 1 for the last NULL char
        return NO_ERROR;
    } else {
        return BAD_VALUE;
    }
}

/*===========================================================================
 * FUNCTION   : getExifLatitude
 *
 * DESCRIPTION: get exif latitude
 *
 * PARAMETERS :
 *   @latitude : ptr to rational struct to store latitude info
 *   @latRef   : character to indicate latitude reference
 *   @value    : value of the latitude
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t getExifLatitude(rat_t *latitude, char *latRef, double value)
{
    char str[30];
    snprintf(str, sizeof(str), "%f", value);
    if(str[0] != '\0') {
        parseGPSCoordinate(str, latitude);

        //set Latitude Ref
        float latitudeValue = strtof(str, 0);
        if(latitudeValue < 0.0f) {
            latRef[0] = 'S';
        } else {
            latRef[0] = 'N';
        }
        latRef[1] = '\0';
        return NO_ERROR;
    }else{
        return BAD_VALUE;
    }
}

/*===========================================================================
 * FUNCTION   : getExifLongitude
 *
 * DESCRIPTION: get exif longitude
 *
 * PARAMETERS :
 *   @longitude : ptr to rational struct to store longitude info
 *   @lonRef    : character to indicate longitude reference
 *   @value     : value of the longitude
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t getExifLongitude(rat_t *longitude, char *lonRef, double value)
{
    char str[30];
    snprintf(str, sizeof(str), "%f", value);
    if(str[0] != '\0') {
        parseGPSCoordinate(str, longitude);

        //set Longitude Ref
        float longitudeValue = strtof(str, 0);
        if(longitudeValue < 0.0f) {
            lonRef[0] = 'W';
        } else {
            lonRef[0] = 'E';
        }
        lonRef[1] = '\0';
        return NO_ERROR;
    }else{
        return BAD_VALUE;
    }
}

/*===========================================================================
 * FUNCTION   : getExifAltitude
 *
 * DESCRIPTION: get exif altitude
 *
 * PARAMETERS :
 *   @altitude : ptr to rational struct to store altitude info
 *   @altRef   : character to indicate altitude reference
 *   @argValue : altitude value
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t getExifAltitude(rat_t *altitude, char *altRef, double argValue)
{
    char str[30];
    snprintf(str, sizeof(str), "%f", argValue);
    if (str[0] != '\0') {
        double value = atof(str);
        *altRef = 0;
        if(value < 0){
            *altRef = 1;
            value = -value;
        }
        return getRational(altitude, (int)(value * 1000), 1000);
    } else {
        return BAD_VALUE;
    }
}

/*===========================================================================
 * FUNCTION   : getExifGpsDateTimeStamp
 *
 * DESCRIPTION: get exif GPS date time stamp
 *
 * PARAMETERS :
 *   @gpsDateStamp : GPS date time stamp string
 *   @bufLen       : length of the string
 *   @gpsTimeStamp : ptr to rational struct to store time stamp info
 *   @value        : timestamp value
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t getExifGpsDateTimeStamp(char *gpsDateStamp, uint32_t bufLen,
        rat_t *gpsTimeStamp, int64_t value)
{
    char str[30];
    snprintf(str, sizeof(str), "%lld", (long long int)value);
    if(str[0] != '\0') {
        time_t unixTime = (time_t)atol(str);
        struct tm *UTCTimestamp = gmtime(&unixTime);
        if (UTCTimestamp != NULL && gpsDateStamp != NULL
                && gpsTimeStamp != NULL) {
            strftime(gpsDateStamp, bufLen, "%Y:%m:%d", UTCTimestamp);

            getRational(&gpsTimeStamp[0], UTCTimestamp->tm_hour, 1);
            getRational(&gpsTimeStamp[1], UTCTimestamp->tm_min, 1);
            getRational(&gpsTimeStamp[2], UTCTimestamp->tm_sec, 1);
            return NO_ERROR;
        } else {
            LOGE("Could not get the timestamp");
            return BAD_VALUE;
        }
    } else {
        return BAD_VALUE;
    }
}

/*===========================================================================
 * FUNCTION   : getExifExposureValue
 *
 * DESCRIPTION: get exif GPS date time stamp
 *
 * PARAMETERS :
 *   @exposure_val        : rational exposure value
 *   @exposure_comp       : exposure compensation
 *   @step                : exposure step
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t getExifExposureValue(srat_t* exposure_val, int32_t exposure_comp,
        cam_rational_type_t step)
{
    exposure_val->num = exposure_comp * step.numerator;
    exposure_val->denom = step.denominator;
    return 0;
}

/*===========================================================================
 * FUNCTION   : getExifData
 *
 * DESCRIPTION: get exif data to be passed into jpeg encoding
 *
 * PARAMETERS :
 * @metadata      : metadata of the encoding request
 * @jpeg_settings : jpeg_settings for encoding
 * @needJpegExifRotation: check if rotation need to added in EXIF
 *
 * RETURN     : exif data from user setting and GPS
 *==========================================================================*/
QCamera3Exif *QCamera3PostProcessor::getExifData(metadata_buffer_t *metadata,
        jpeg_settings_t *jpeg_settings, bool needJpegExifRotation)
{
    QCamera3Exif *exif = new QCamera3Exif();
    if (exif == NULL) {
        LOGE("No memory for QCamera3Exif");
        return NULL;
    }
    if (m_parent == NULL) {
        LOGE("m_parent is NULL, Error");
        delete exif;
        return NULL;
    }

    int32_t rc = NO_ERROR;
    uint32_t count = 0;

    // Real EXIF UserComment entry for org.lineageos.camera.algorithms.exifComment.
    // The still processor already snapshotted the UTF-8 payload at submission
    // time, so the encoded JPEG carries exactly the bytes the request supplied.
    // The post-encode carrier (QCamera3ExifExtras, which can also grow an APP1
    // segment) is not used here: the JPEG output record exposes no destination
    // capacity, so a growing rewrite could not be proven to fit.
    if (jpeg_settings != NULL && jpeg_settings->native_stages.exif_comment_length > 0 &&
            jpeg_settings->native_stages.exif_comment_length <
                int32_t(sizeof(jpeg_settings->native_stages.exif_comment))) {
        const int32_t commentLength = jpeg_settings->native_stages.exif_comment_length;
        exif->addEntry(EXIFTAGID_EXIF_USER_COMMENT, EXIFTAGTYPE_EXIF_USER_COMMENT,
                uint32_t(commentLength + 1),
                (void *)jpeg_settings->native_stages.exif_comment);
    }

    // add exif entries
    String8 dateTime;
    String8 subsecTime;
    rc = getExifDateTime(dateTime, subsecTime);
    if (rc == NO_ERROR) {
        exif->addEntry(EXIFTAGID_DATE_TIME, EXIF_ASCII,
                (uint32_t)(dateTime.length() + 1), (void *)dateTime.c_str());
        exif->addEntry(EXIFTAGID_EXIF_DATE_TIME_ORIGINAL, EXIF_ASCII,
                (uint32_t)(dateTime.length() + 1), (void *)dateTime.c_str());
        exif->addEntry(EXIFTAGID_EXIF_DATE_TIME_DIGITIZED, EXIF_ASCII,
                (uint32_t)(dateTime.length() + 1), (void *)dateTime.c_str());
        exif->addEntry(EXIFTAGID_SUBSEC_TIME, EXIF_ASCII,
                (uint32_t)(subsecTime.length() + 1), (void *)subsecTime.c_str());
        exif->addEntry(EXIFTAGID_SUBSEC_TIME_ORIGINAL, EXIF_ASCII,
                (uint32_t)(subsecTime.length() + 1), (void *)subsecTime.c_str());
        exif->addEntry(EXIFTAGID_SUBSEC_TIME_DIGITIZED, EXIF_ASCII,
                (uint32_t)(subsecTime.length() + 1), (void *)subsecTime.c_str());
    } else {
        LOGW("getExifDateTime failed");
    }


    if (metadata != NULL) {
        IF_META_AVAILABLE(float, focal_length, CAM_INTF_META_LENS_FOCAL_LENGTH, metadata) {
            rat_t focalLength;
            rc = getExifFocalLength(&focalLength, *focal_length);
            if (rc == NO_ERROR) {
                exif->addEntry(EXIFTAGID_FOCAL_LENGTH,
                        EXIF_RATIONAL,
                        1,
                        (void *)&(focalLength));
            } else {
                LOGW("getExifFocalLength failed");
            }
        }

        char* jpeg_gps_processing_method = jpeg_settings->gps_processing_method;
        if (strlen(jpeg_gps_processing_method) > 0) {
            char gpsProcessingMethod[EXIF_ASCII_PREFIX_SIZE +
                    GPS_PROCESSING_METHOD_SIZE];
            count = 0;
            rc = getExifGpsProcessingMethod(gpsProcessingMethod,
                    count,
                    jpeg_gps_processing_method);
            if(rc == NO_ERROR) {
                exif->addEntry(EXIFTAGID_GPS_PROCESSINGMETHOD,
                        EXIFTAGTYPE_GPS_PROCESSINGMETHOD,
                        count,
                        (void *)gpsProcessingMethod);
            } else {
                LOGW("getExifGpsProcessingMethod failed");
            }
        }

        if (jpeg_settings->gps_coordinates_valid) {

            //latitude
            rat_t latitude[3];
            char latRef[2];
            rc = getExifLatitude(latitude, latRef,
                    jpeg_settings->gps_coordinates[0]);
            if(rc == NO_ERROR) {
                exif->addEntry(EXIFTAGID_GPS_LATITUDE,
                        EXIF_RATIONAL,
                        3,
                        (void *)latitude);
                exif->addEntry(EXIFTAGID_GPS_LATITUDE_REF,
                        EXIF_ASCII,
                        2,
                        (void *)latRef);
            } else {
                LOGW("getExifLatitude failed");
            }

            //longitude
            rat_t longitude[3];
            char lonRef[2];
            rc = getExifLongitude(longitude, lonRef,
                    jpeg_settings->gps_coordinates[1]);
            if(rc == NO_ERROR) {
                exif->addEntry(EXIFTAGID_GPS_LONGITUDE,
                        EXIF_RATIONAL,
                        3,
                        (void *)longitude);

                exif->addEntry(EXIFTAGID_GPS_LONGITUDE_REF,
                        EXIF_ASCII,
                        2,
                        (void *)lonRef);
            } else {
                LOGW("getExifLongitude failed");
            }

            //altitude
            rat_t altitude;
            char altRef;
            rc = getExifAltitude(&altitude, &altRef,
                    jpeg_settings->gps_coordinates[2]);
            if(rc == NO_ERROR) {
                exif->addEntry(EXIFTAGID_GPS_ALTITUDE,
                        EXIF_RATIONAL,
                        1,
                        (void *)&(altitude));

                exif->addEntry(EXIFTAGID_GPS_ALTITUDE_REF,
                        EXIF_BYTE,
                        1,
                        (void *)&altRef);
            } else {
                LOGW("getExifAltitude failed");
            }
        }

        if (jpeg_settings->gps_timestamp_valid) {

            char gpsDateStamp[20];
            rat_t gpsTimeStamp[3];
            rc = getExifGpsDateTimeStamp(gpsDateStamp, 20, gpsTimeStamp,
                    jpeg_settings->gps_timestamp);
            if(rc == NO_ERROR) {
                exif->addEntry(EXIFTAGID_GPS_DATESTAMP, EXIF_ASCII,
                        (uint32_t)(strlen(gpsDateStamp) + 1),
                        (void *)gpsDateStamp);

                exif->addEntry(EXIFTAGID_GPS_TIMESTAMP,
                        EXIF_RATIONAL,
                        3,
                        (void *)gpsTimeStamp);
            } else {
                LOGW("getExifGpsDataTimeStamp failed");
            }
        }

        IF_META_AVAILABLE(int32_t, exposure_comp, CAM_INTF_PARM_EXPOSURE_COMPENSATION, metadata) {
            IF_META_AVAILABLE(cam_rational_type_t, comp_step, CAM_INTF_PARM_EV_STEP, metadata) {
                srat_t exposure_val;
                rc = getExifExposureValue(&exposure_val, *exposure_comp, *comp_step);
                if(rc == NO_ERROR) {
                    exif->addEntry(EXIFTAGID_EXPOSURE_BIAS_VALUE,
                            EXIF_SRATIONAL,
                            1,
                            (void *)(&exposure_val));
                } else {
                    LOGW("getExifExposureValue failed ");
                }
            }
        }
    } else {
        LOGW("no metadata provided ");
    }

#ifdef ENABLE_MODEL_INFO_EXIF

    char value[PROPERTY_VALUE_MAX];
    if (property_get("ro.product.manufacturer", value, "QCOM-AA") > 0) {
        exif->addEntry(EXIFTAGID_MAKE, EXIF_ASCII,
                (uint32_t)(strlen(value) + 1), (void *)value);
    } else {
        LOGW("getExifMaker failed");
    }

    if (property_get("ro.product.model", value, "QCAM-AA") > 0) {
        exif->addEntry(EXIFTAGID_MODEL, EXIF_ASCII,
                (uint32_t)(strlen(value) + 1), (void *)value);
    } else {
        LOGW("getExifModel failed");
    }

    if (property_get("ro.build.description", value, "QCAM-AA") > 0) {
        exif->addEntry(EXIFTAGID_SOFTWARE, EXIF_ASCII,
                (uint32_t)(strlen(value) + 1), (void *)value);
    } else {
        LOGW("getExifSoftware failed");
    }

#endif

    if (jpeg_settings->image_desc_valid) {
        if (exif->addEntry(EXIFTAGID_IMAGE_DESCRIPTION, EXIF_ASCII,
                strlen(jpeg_settings->image_desc)+1,
                (void *)jpeg_settings->image_desc)) {
            LOGW("Adding IMAGE_DESCRIPTION tag failed");
        }
    }

    LOGD("needJpegExifRotation %d jpeg_settings->jpeg_orientation %d",
            needJpegExifRotation, jpeg_settings->jpeg_orientation);

    if (needJpegExifRotation) {
        int16_t orientation;
        switch (jpeg_settings->jpeg_orientation) {
            case 0:
                orientation = 1;
                break;
            case 90:
                orientation = 6;
                break;
            case 180:
                orientation = 3;
                break;
            case 270:
                orientation = 8;
                break;
            default:
                orientation = 1;
                break;
        }
        exif->addEntry(EXIFTAGID_ORIENTATION,
                       EXIF_SHORT,
                       1,
                       (void *)&orientation);
        exif->addEntry(EXIFTAGID_TN_ORIENTATION,
                       EXIF_SHORT,
                       1,
                       (void *)&orientation);

    }

    return exif;
}

/*===========================================================================
 * FUNCTION   : QCamera3Exif
 *
 * DESCRIPTION: constructor of QCamera3Exif
 *
 * PARAMETERS : None
 *
 * RETURN     : None
 *==========================================================================*/
QCamera3Exif::QCamera3Exif()
    : m_nNumEntries(0)
{
    memset(m_Entries, 0, sizeof(m_Entries));
}

/*===========================================================================
 * FUNCTION   : ~QCamera3Exif
 *
 * DESCRIPTION: deconstructor of QCamera3Exif. Will release internal memory ptr.
 *
 * PARAMETERS : None
 *
 * RETURN     : None
 *==========================================================================*/
QCamera3Exif::~QCamera3Exif()
{
    for (uint32_t i = 0; i < m_nNumEntries; i++) {
        switch (m_Entries[i].tag_entry.type) {
            case EXIF_BYTE:
                {
                    if (m_Entries[i].tag_entry.count > 1 &&
                            m_Entries[i].tag_entry.data._bytes != NULL) {
                        free(m_Entries[i].tag_entry.data._bytes);
                        m_Entries[i].tag_entry.data._bytes = NULL;
                    }
                }
                break;
            case EXIF_ASCII:
                {
                    if (m_Entries[i].tag_entry.data._ascii != NULL) {
                        free(m_Entries[i].tag_entry.data._ascii);
                        m_Entries[i].tag_entry.data._ascii = NULL;
                    }
                }
                break;
            case EXIF_SHORT:
                {
                    if (m_Entries[i].tag_entry.count > 1 &&
                            m_Entries[i].tag_entry.data._shorts != NULL) {
                        free(m_Entries[i].tag_entry.data._shorts);
                        m_Entries[i].tag_entry.data._shorts = NULL;
                    }
                }
                break;
            case EXIF_LONG:
                {
                    if (m_Entries[i].tag_entry.count > 1 &&
                            m_Entries[i].tag_entry.data._longs != NULL) {
                        free(m_Entries[i].tag_entry.data._longs);
                        m_Entries[i].tag_entry.data._longs = NULL;
                    }
                }
                break;
            case EXIF_RATIONAL:
                {
                    if (m_Entries[i].tag_entry.count > 1 &&
                            m_Entries[i].tag_entry.data._rats != NULL) {
                        free(m_Entries[i].tag_entry.data._rats);
                        m_Entries[i].tag_entry.data._rats = NULL;
                    }
                }
                break;
            case EXIF_UNDEFINED:
                {
                    if (m_Entries[i].tag_entry.data._undefined != NULL) {
                        free(m_Entries[i].tag_entry.data._undefined);
                        m_Entries[i].tag_entry.data._undefined = NULL;
                    }
                }
                break;
            case EXIF_SLONG:
                {
                    if (m_Entries[i].tag_entry.count > 1 &&
                            m_Entries[i].tag_entry.data._slongs != NULL) {
                        free(m_Entries[i].tag_entry.data._slongs);
                        m_Entries[i].tag_entry.data._slongs = NULL;
                    }
                }
                break;
            case EXIF_SRATIONAL:
                {
                    if (m_Entries[i].tag_entry.count > 1 &&
                            m_Entries[i].tag_entry.data._srats != NULL) {
                        free(m_Entries[i].tag_entry.data._srats);
                        m_Entries[i].tag_entry.data._srats = NULL;
                    }
                }
                break;
            default:
                LOGW("Error, Unknown type");
                break;
        }
    }
}

/*===========================================================================
 * FUNCTION   : addEntry
 *
 * DESCRIPTION: function to add an entry to exif data
 *
 * PARAMETERS :
 *   @tagid   : exif tag ID
 *   @type    : data type
 *   @count   : number of data in uint of its type
 *   @data    : input data ptr
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t QCamera3Exif::addEntry(exif_tag_id_t tagid,
                              exif_tag_type_t type,
                              uint32_t count,
                              void *data)
{
    int32_t rc = NO_ERROR;
    if(m_nNumEntries >= MAX_HAL3_EXIF_TABLE_ENTRIES) {
        LOGE("Number of entries exceeded limit");
        return NO_MEMORY;
    }

    m_Entries[m_nNumEntries].tag_id = tagid;
    m_Entries[m_nNumEntries].tag_entry.type = type;
    m_Entries[m_nNumEntries].tag_entry.count = count;
    m_Entries[m_nNumEntries].tag_entry.copy = 1;
    switch (type) {
        case EXIF_BYTE:
            {
                if (count > 1) {
                    uint8_t *values = (uint8_t *)malloc(count);
                    if (values == NULL) {
                        LOGE("No memory for byte array");
                        rc = NO_MEMORY;
                    } else {
                        memcpy(values, data, count);
                        m_Entries[m_nNumEntries].tag_entry.data._bytes = values;
                    }
                } else {
                    m_Entries[m_nNumEntries].tag_entry.data._byte =
                        *(uint8_t *)data;
                }
            }
            break;
        case EXIF_ASCII:
            {
                char *str = NULL;
                str = (char *)malloc(count + 1);
                if (str == NULL) {
                    LOGE("No memory for ascii string");
                    rc = NO_MEMORY;
                } else {
                    memset(str, 0, count + 1);
                    memcpy(str, data, count);
                    m_Entries[m_nNumEntries].tag_entry.data._ascii = str;
                }
            }
            break;
        case EXIF_SHORT:
            {
                uint16_t *exif_data = (uint16_t *)data;
                if (count > 1) {
                    uint16_t *values =
                        (uint16_t *)malloc(count * sizeof(uint16_t));
                    if (values == NULL) {
                        LOGE("No memory for short array");
                        rc = NO_MEMORY;
                    } else {
                        memcpy(values, exif_data, count * sizeof(uint16_t));
                        m_Entries[m_nNumEntries].tag_entry.data._shorts = values;
                    }
                } else {
                    m_Entries[m_nNumEntries].tag_entry.data._short =
                        *(uint16_t *)data;
                }
            }
            break;
        case EXIF_LONG:
            {
                uint32_t *exif_data = (uint32_t *)data;
                if (count > 1) {
                    uint32_t *values =
                        (uint32_t *)malloc(count * sizeof(uint32_t));
                    if (values == NULL) {
                        LOGE("No memory for long array");
                        rc = NO_MEMORY;
                    } else {
                        memcpy(values, exif_data, count * sizeof(uint32_t));
                        m_Entries[m_nNumEntries].tag_entry.data._longs = values;
                    }
                } else {
                    m_Entries[m_nNumEntries].tag_entry.data._long =
                        *(uint32_t *)data;
                }
            }
            break;
        case EXIF_RATIONAL:
            {
                rat_t *exif_data = (rat_t *)data;
                if (count > 1) {
                    rat_t *values = (rat_t *)malloc(count * sizeof(rat_t));
                    if (values == NULL) {
                        LOGE("No memory for rational array");
                        rc = NO_MEMORY;
                    } else {
                        memcpy(values, exif_data, count * sizeof(rat_t));
                        m_Entries[m_nNumEntries].tag_entry.data._rats = values;
                    }
                } else {
                    m_Entries[m_nNumEntries].tag_entry.data._rat =
                        *(rat_t *)data;
                }
            }
            break;
        case EXIF_UNDEFINED:
            {
                uint8_t *values = (uint8_t *)malloc(count);
                if (values == NULL) {
                    LOGE("No memory for undefined array");
                    rc = NO_MEMORY;
                } else {
                    memcpy(values, data, count);
                    m_Entries[m_nNumEntries].tag_entry.data._undefined = values;
                }
            }
            break;
        case EXIF_SLONG:
            {
                int32_t *exif_data = (int32_t *)data;
                if (count > 1) {
                    int32_t *values =
                        (int32_t *)malloc(count * sizeof(int32_t));
                    if (values == NULL) {
                        LOGE("No memory for signed long array");
                        rc = NO_MEMORY;
                    } else {
                        memcpy(values, exif_data, count * sizeof(int32_t));
                        m_Entries[m_nNumEntries].tag_entry.data._slongs =values;
                    }
                } else {
                    m_Entries[m_nNumEntries].tag_entry.data._slong =
                        *(int32_t *)data;
                }
            }
            break;
        case EXIF_SRATIONAL:
            {
                srat_t *exif_data = (srat_t *)data;
                if (count > 1) {
                    srat_t *values = (srat_t *)malloc(count * sizeof(srat_t));
                    if (values == NULL) {
                        LOGE("No memory for sign rational array");
                        rc = NO_MEMORY;
                    } else {
                        memcpy(values, exif_data, count * sizeof(srat_t));
                        m_Entries[m_nNumEntries].tag_entry.data._srats = values;
                    }
                } else {
                    m_Entries[m_nNumEntries].tag_entry.data._srat =
                        *(srat_t *)data;
                }
            }
            break;
        default:
            LOGE("Error, Unknown type");
            break;
    }

    // Increase number of entries
    m_nNumEntries++;
    return rc;
}

}; // namespace qcamera
