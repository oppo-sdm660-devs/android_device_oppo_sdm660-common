/* Copyright (c) 2012-2016, The Linux Foundation. All rights reserved.
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

// System dependencies
#include <string.h>
#include <errno.h>
#include <utils/Errors.h>
#define PRCTL_H <SYSTEM_HEADER_PREFIX/prctl.h>
#include PRCTL_H

// Camera dependencies
#include "QCameraCmdThread.h"

extern "C" {
#include "mm_camera_dbg.h"
}

using namespace android;

namespace qcamera {
namespace {
struct CommandLock {
    pthread_mutex_t *mutex;
    explicit CommandLock(pthread_mutex_t *lock) : mutex(lock) {
        if (mutex) pthread_mutex_lock(mutex);
    }
    ~CommandLock() { if (mutex) pthread_mutex_unlock(mutex); }
};
}

/*===========================================================================
 * FUNCTION   : QCameraCmdThread
 *
 * DESCRIPTION: default constructor of QCameraCmdThread
 *
 * PARAMETERS : None
 *
 * RETURN     : None
 *==========================================================================*/
QCameraCmdThread::QCameraCmdThread() :
    cmd_queue(), mExitRequested(false), mStopRequested(false)
{
    cmd_pid = 0;
    pthread_mutex_init(&mSyncCommandLock, NULL);
    pthread_mutex_init(&mThreadLock, NULL);
    cam_sem_init(&sync_sem, 0);
    cam_sem_init(&cmd_sem, 0);
}

/*===========================================================================
 * FUNCTION   : ~QCameraCmdThread
 *
 * DESCRIPTION: deconstructor of QCameraCmdThread
 *
 * PARAMETERS : None
 *
 * RETURN     : None
 *==========================================================================*/
QCameraCmdThread::~QCameraCmdThread()
{
    exit();
    cam_sem_destroy(&sync_sem);
    cam_sem_destroy(&cmd_sem);
    pthread_mutex_destroy(&mSyncCommandLock);
    pthread_mutex_destroy(&mThreadLock);
}

/*===========================================================================
 * FUNCTION   : launch
 *
 * DESCRIPTION: launch Cmd Thread
 *
 * PARAMETERS :
 *   @start_routine : thread routine function ptr
 *   @user_data     : user data ptr
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t QCameraCmdThread::launch(void *(*start_routine)(void *),
                                 void* user_data)
{
    CommandLock syncLock(&mSyncCommandLock);
    CommandLock threadLock(&mThreadLock);
    if (cmd_pid != 0) return INVALID_OPERATION;
    mExitRequested.store(false);
    mStopRequested.store(false);
    cmd_queue.init();
    int rc = pthread_create(&cmd_pid, NULL, start_routine, user_data);
    if (rc != 0) {
        cmd_pid = 0;
        return -rc;
    }
    return NO_ERROR;
}

/*===========================================================================
 * FUNCTION   : setName
 *
 * DESCRIPTION: name the cmd thread
 *
 * PARAMETERS :
 *   @name : desired name for the thread
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t QCameraCmdThread::setName(const char* name)
{
    /* name the thread */
    prctl(PR_SET_NAME, (unsigned long)name, 0, 0, 0);
    return NO_ERROR;
}

/*===========================================================================
 * FUNCTION   : sendCmd
 *
 * DESCRIPTION: send a command to the Cmd Thread
 *
 * PARAMETERS :
 *   @cmd     : command to be executed.
 *   @sync_cmd: flag to indicate if this is a synchorinzed cmd. If true, this call
 *              will wait until signal is set after the command is completed.
 *   @priority: flag to indicate if this is a cmd with priority. If true, the cmd
 *              will be enqueued to the head with priority.
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t QCameraCmdThread::sendCmd(camera_cmd_type_t cmd, uint8_t sync_cmd, uint8_t priority)
{
    // Only synchronous senders contend for acknowledgements. Async callback
    // sends must remain possible while the worker completes a sync command.
    CommandLock syncLock(sync_cmd ? &mSyncCommandLock : NULL);
    pthread_mutex_lock(&mThreadLock);
    if (cmd_pid == 0 || mExitRequested.load()) {
        pthread_mutex_unlock(&mThreadLock);
        return -ENODEV;
    }
    camera_cmd_t *node = (camera_cmd_t *)malloc(sizeof(camera_cmd_t));
    if (NULL == node) {
        LOGE("No memory for camera_cmd_t");
        if (cmd == CAMERA_CMD_TYPE_STOP_DATA_PROC && !sync_cmd) {
            mStopRequested.store(true);
            cam_sem_post(&cmd_sem);
            pthread_mutex_unlock(&mThreadLock);
            return NO_ERROR;
        }
        pthread_mutex_unlock(&mThreadLock);
        return NO_MEMORY;
    }
    memset(node, 0, sizeof(camera_cmd_t));
    node->cmd = cmd;
    node->is_sync = sync_cmd;

    if (priority) {
        if (!cmd_queue.enqueueWithPriority((void *)node)) {
            free(node);
            node = NULL;
        }
    } else {
        if (!cmd_queue.enqueue((void *)node)) {
            free(node);
            node = NULL;
        }
    }
    if (node == NULL) {
        if (cmd == CAMERA_CMD_TYPE_STOP_DATA_PROC && !sync_cmd) {
            mStopRequested.store(true);
            cam_sem_post(&cmd_sem);
            pthread_mutex_unlock(&mThreadLock);
            return NO_ERROR;
        }
        pthread_mutex_unlock(&mThreadLock);
        return NO_MEMORY;
    }
    cam_sem_post(&cmd_sem);
    pthread_mutex_unlock(&mThreadLock);

    /* if is a sync call, need to wait until it returns */
    if (sync_cmd) {
        cam_sem_wait(&sync_sem);
    }
    return NO_ERROR;
}

/*===========================================================================
 * FUNCTION   : getCmd
 *
 * DESCRIPTION: dequeue a cmommand from cmd queue
 *
 * PARAMETERS : None
 *
 * RETURN     : cmd dequeued
 *==========================================================================*/
camera_cmd_type_t QCameraCmdThread::getCmd()
{
    if (mExitRequested.load()) return CAMERA_CMD_TYPE_EXIT;
    if (mStopRequested.exchange(false)) return CAMERA_CMD_TYPE_STOP_DATA_PROC;
    camera_cmd_type_t cmd = CAMERA_CMD_TYPE_NONE;
    camera_cmd_t *node = (camera_cmd_t *)cmd_queue.dequeue();
    if (NULL == node) {
        LOGD("No notify avail");
        return CAMERA_CMD_TYPE_NONE;
    } else {
        cmd = node->cmd;
        free(node);
    }
    return cmd;
}

/*===========================================================================
 * FUNCTION   : getCmd
 *
 * DESCRIPTION: dequeue a cmommand from cmd queue
 *
 * PARAMETERS :
 *   @uint8_t : sync flag for sending post if cmd wait.
 *
 * RETURN     : cmd dequeued
 *==========================================================================*/
camera_cmd_type_t QCameraCmdThread::getCmd(uint8_t &sync_cmd)
{
    sync_cmd = 0;
    if (mExitRequested.load()) return CAMERA_CMD_TYPE_EXIT;
    if (mStopRequested.exchange(false)) return CAMERA_CMD_TYPE_STOP_DATA_PROC;
    camera_cmd_type_t cmd = CAMERA_CMD_TYPE_NONE;
    camera_cmd_t *node = (camera_cmd_t *)cmd_queue.dequeue();
    if (NULL == node) {
        LOGD("No notify avail");
        return CAMERA_CMD_TYPE_NONE;
    } else {
        cmd = node->cmd;
        sync_cmd = node->is_sync;
        free(node);
    }
    return cmd;
}

/*===========================================================================
 * FUNCTION   : exit
 *
 * DESCRIPTION: exit the CMD thread
 *
 * PARAMETERS : None
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t QCameraCmdThread::exit()
{
    CommandLock syncLock(&mSyncCommandLock);
    int32_t rc = NO_ERROR;
    pthread_mutex_lock(&mThreadLock);
    if (cmd_pid == 0) {
        pthread_mutex_unlock(&mThreadLock);
        return rc;
    }

    // Shutdown must work even when command/node allocation fails.
    mExitRequested.store(true);
    cam_sem_post(&cmd_sem);
    pthread_t exitingThread = cmd_pid;
    pthread_mutex_unlock(&mThreadLock);

    /* wait until cmd thread exits */
    if (pthread_join(exitingThread, NULL) != 0) {
        LOGD("pthread dead already\n");
    }
    pthread_mutex_lock(&mThreadLock);
    cmd_pid = 0;
    cmd_queue.flush();
    pthread_mutex_unlock(&mThreadLock);
    return rc;
}

}; // namespace qcamera
