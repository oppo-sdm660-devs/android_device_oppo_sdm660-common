/* Copyright (c) 2015-2016, The Linux Foundation. All rights reserved.
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
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <media/msm_cam_sensor.h>

// Camera dependencies
#include "HAL3/QCamera3HWI.h"
#include "QCameraFlash.h"

extern "C" {
#include "mm_camera_dbg.h"
}

#define STRING_LENGTH_OF_64_BIT_NUMBER 21

volatile uint32_t gCamHal3LogLevel = 1;

namespace qcamera {

/*===========================================================================
 * FUNCTION   : getInstance
 *
 * DESCRIPTION: Get and create the QCameraFlash singleton.
 *
 * PARAMETERS : None
 *
 * RETURN     : None
 *==========================================================================*/
QCameraFlash& QCameraFlash::getInstance()
{
    static QCameraFlash flashInstance;
    return flashInstance;
}

/*===========================================================================
 * FUNCTION   : QCameraFlash
 *
 * DESCRIPTION: default constructor of QCameraFlash
 *
 * PARAMETERS : None
 *
 * RETURN     : None
 *==========================================================================*/
QCameraFlash::QCameraFlash() : m_dispatching(false), m_callbacks(NULL)
{
    memset(&m_flashOn, 0, sizeof(m_flashOn));
    memset(&m_flashModeValid, 0, sizeof(m_flashModeValid));
    memset(&m_cameraOpen, 0, sizeof(m_cameraOpen));
    for (int pos = 0; pos < MM_CAMERA_MAX_NUM_SENSORS; pos++) {
        m_flashFds[pos] = -1;
    }
}

/*===========================================================================
 * FUNCTION   : ~QCameraFlash
 *
 * DESCRIPTION: deconstructor of QCameraFlash
 *
 * PARAMETERS : None
 *
 * RETURN     : None
 *==========================================================================*/
QCameraFlash::~QCameraFlash()
{
    android::Mutex::Autolock lock(m_lock);
    for (int pos = 0; pos < MM_CAMERA_MAX_NUM_SENSORS; pos++) {
        if (m_flashFds[pos] >= 0) {
            if (deinitFlashLocked(pos) != 0) {
                close(m_flashFds[pos]);
                m_flashFds[pos] = -1;
            }
        }
    }
}

/*===========================================================================
 * FUNCTION   : registerCallbacks
 *
 * DESCRIPTION: provide flash module with reference to callbacks to framework
 *
 * PARAMETERS : None
 *
 * RETURN     : None
 *==========================================================================*/
int32_t QCameraFlash::registerCallbacks(
        const camera_module_callbacks_t* callbacks)
{
    android::Mutex::Autolock lock(m_lock);
    int32_t retVal = 0;
    m_callbacks = callbacks;
    return retVal;
}

/* Callbacks run outside m_lock; reentrant events stay queued for this dispatcher. */
void QCameraFlash::dispatchCallbacks()
{
    {
        android::Mutex::Autolock lock(m_lock);
        if (m_dispatching) {
            return;
        }
        m_dispatching = true;
    }
    for (;;) {
        TorchEvent event;
        const camera_module_callbacks_t *callbacks;
        {
            android::Mutex::Autolock lock(m_lock);
            if (m_pendingCallbacks.empty()) {
                m_dispatching = false;
                return;
            }
            event = m_pendingCallbacks.front();
            m_pendingCallbacks.pop_front();
            callbacks = m_callbacks;
        }
        if (callbacks != NULL && callbacks->torch_mode_status_change != NULL) {
            char cameraIdStr[STRING_LENGTH_OF_64_BIT_NUMBER];
            snprintf(cameraIdStr, sizeof(cameraIdStr), "%d", event.cameraId);
            callbacks->torch_mode_status_change(callbacks, cameraIdStr, event.status);
        }
    }
}

/* Main and auxiliary camera IDs can share a flash node. */
bool QCameraFlash::sharesFlashLocked(const int first, const int second)
{
    bool firstHasFlash = false;
    bool secondHasFlash = false;
    char firstNode[QCAMERA_MAX_FILEPATH_LENGTH];
    char secondNode[QCAMERA_MAX_FILEPATH_LENGTH];
    QCamera3HardwareInterface::getFlashInfo(first, firstHasFlash, firstNode);
    QCamera3HardwareInterface::getFlashInfo(second, secondHasFlash, secondNode);
    return firstHasFlash && secondHasFlash && firstNode[0] != '\0' &&
            strcmp(firstNode, secondNode) == 0;
}

int QCameraFlash::cameraOwnerLocked(const int camera_id)
{
    for (int id = 0; id < MM_CAMERA_MAX_NUM_SENSORS; ++id) {
        if (m_cameraOpen[id] && sharesFlashLocked(camera_id, id)) {
            return id;
        }
    }
    return -1;
}

int QCameraFlash::torchOwnerLocked(const int camera_id)
{
    for (int id = 0; id < MM_CAMERA_MAX_NUM_SENSORS; ++id) {
        if (m_flashFds[id] >= 0 && sharesFlashLocked(camera_id, id)) {
            return id;
        }
    }
    return -1;
}

void QCameraFlash::queueGroupStatusLocked(const int camera_id, const int status)
{
    for (int id = 0; id < MM_CAMERA_MAX_NUM_SENSORS; ++id) {
        if (sharesFlashLocked(camera_id, id)) {
            TorchEvent event = { id, status };
            m_pendingCallbacks.push_back(event);
        }
    }
}

int32_t QCameraFlash::setTorchMode(const int camera_id, const bool on)
{
    int32_t retVal;
    {
        android::Mutex::Autolock lock(m_lock);
        if (camera_id < 0 || camera_id >= MM_CAMERA_MAX_NUM_SENSORS) {
            return -EINVAL;
        }
        bool hasFlash = false;
        char flashNode[QCAMERA_MAX_FILEPATH_LENGTH];
        QCamera3HardwareInterface::getFlashInfo(camera_id, hasFlash, flashNode);
        if (!hasFlash) {
            return -ENOSYS;
        }
        if (flashNode[0] == '\0') {
            return -ENODEV;
        }
        int cameraOwner = cameraOwnerLocked(camera_id);
        if (cameraOwner >= 0) {
            return cameraOwner == camera_id ? -EBUSY : -EUSERS;
        }
        int torchOwner = torchOwnerLocked(camera_id);
        if (on) {
            retVal = 0;
            if (torchOwner >= 0 && torchOwner != camera_id) {
                retVal = deinitFlashLocked(torchOwner);
                if (retVal == 0) {
                    TorchEvent event = { torchOwner, TORCH_MODE_STATUS_AVAILABLE_OFF };
                    m_pendingCallbacks.push_back(event);
                }
            }
            if (retVal == 0) {
                retVal = initFlashLocked(camera_id);
            }
            if (retVal == 0) {
                retVal = setFlashModeLocked(camera_id, true);
                if (retVal == -EALREADY) {
                    retVal = 0;
                } else if (retVal != 0) {
                    int32_t cleanup = deinitFlashLocked(camera_id);
                    if (cleanup != 0) {
                        LOGE("Failed to release flash after torch error: %d", cleanup);
                    }
                }
            }
        } else {
            // OFF targets the physical unit even when a sibling ID owns it.
            retVal = torchOwner >= 0 ? deinitFlashLocked(torchOwner) : 0;
            if (retVal == 0 && torchOwner >= 0 && torchOwner != camera_id) {
                TorchEvent event = { torchOwner, TORCH_MODE_STATUS_AVAILABLE_OFF };
                m_pendingCallbacks.push_back(event);
            }
        }
        if (retVal == 0) {
            TorchEvent event = { camera_id,
                    on ? TORCH_MODE_STATUS_AVAILABLE_ON : TORCH_MODE_STATUS_AVAILABLE_OFF };
            m_pendingCallbacks.push_back(event);
        }
    }
    dispatchCallbacks();
    return retVal;
}

/*===========================================================================
 * FUNCTION   : initFlash
 *
 * DESCRIPTION: Reserve and initialize the flash unit associated with a
 *              given camera id. This function is blocking until the
 *              operation completes or fails. Each flash unit can be "inited"
 *              by only one process at a time.
 *
 * PARAMETERS :
 *   @camera_id : Camera id of the flash.
 *
 * RETURN     :
 *   0        : success
 *   -EBUSY   : The flash unit or the resource needed to turn on the
 *              the flash is busy, typically because the flash is
 *              already in use.
 *   -EINVAL  : Invalid camera_id.
 *   -ENOSYS  : No flash present at camera_id.
 *==========================================================================*/
int32_t QCameraFlash::initFlashLocked(const int camera_id)
{
    int32_t retVal = 0;
    bool hasFlash = false;
    char flashNode[QCAMERA_MAX_FILEPATH_LENGTH];
    char flashPath[QCAMERA_MAX_FILEPATH_LENGTH] = "/dev/";

    if (camera_id < 0 || camera_id >= MM_CAMERA_MAX_NUM_SENSORS) {
        LOGE("Invalid camera id: %d", camera_id);
        return -EINVAL;
    }

    QCamera3HardwareInterface::getFlashInfo(camera_id,
            hasFlash,
            flashNode);

    strlcat(flashPath,
            flashNode,
            sizeof(flashPath));

    if (!hasFlash) {
        LOGE("No flash available for camera id: %d",
                camera_id);
        retVal = -ENOSYS;
    } else if (m_cameraOpen[camera_id]) {
        LOGE("Camera in use for camera id: %d",
                camera_id);
        retVal = -EBUSY;
    } else {
        if (m_flashFds[camera_id] >= 0 &&
                (!m_flashOn[camera_id] || !m_flashModeValid[camera_id])) {
            retVal = deinitFlashLocked(camera_id);
            if (retVal != 0) {
                return retVal;
            }
        }
        if (m_flashFds[camera_id] >= 0) {
            LOGD("Flash is already inited for camera id: %d",
                    camera_id);
            return 0;
        }
        m_flashFds[camera_id] = open(flashPath, O_RDWR | O_NONBLOCK);

        if (m_flashFds[camera_id] < 0) {
            LOGE("Unable to open node '%s'",
                    flashPath);
            retVal = -EBUSY;
        } else {
            struct msm_flash_cfg_data_t cfg;
            struct msm_flash_init_info_t init_info;
            memset(&cfg, 0, sizeof(struct msm_flash_cfg_data_t));
            memset(&init_info, 0, sizeof(struct msm_flash_init_info_t));
            init_info.flash_driver_type = FLASH_DRIVER_DEFAULT;
            cfg.cfg.flash_init_info = &init_info;
            cfg.cfg_type = CFG_FLASH_INIT;
            m_flashModeValid[camera_id] = false;
            retVal = ioctl(m_flashFds[camera_id],
                    VIDIOC_MSM_FLASH_CFG,
                    &cfg);
            if (retVal < 0) {
                retVal = -errno;
                LOGE("Unable to init flash for camera id: %d",
                        camera_id);
                // INIT can fail after changing hardware state.
                int32_t cleanup = deinitFlashLocked(camera_id);
                if (cleanup != 0) {
                    LOGE("Failed to release flash after init error: %d", cleanup);
                }
            }

            /* wait for PMIC to init */
            usleep(5000);
        }
    }

    LOGD("X, retVal = %d", retVal);
    return retVal;
}

/*===========================================================================
 * FUNCTION   : setFlashMode
 *
 * DESCRIPTION: Turn on or off the flash associated with a given handle.
 *              This function is blocking until the operation completes or
 *              fails.
 *
 * PARAMETERS :
 *   @camera_id  : Camera id of the flash
 *   @on         : Whether to turn flash on (true) or off (false)
 *
 * RETURN     :
 *   0        : success
 *   -EINVAL  : No camera present at camera_id, or it is not inited.
 *   -ENOSYS  : No flash present at camera_id.
 *   -EBUSY   : The camera already owns the flash.
 *   -EALREADY: Flash is already in requested state
 *==========================================================================*/
int32_t QCameraFlash::setFlashModeLocked(const int camera_id, const bool mode)
{
    int32_t retVal = 0;
    struct msm_flash_cfg_data_t cfg;

    if (camera_id < 0 || camera_id >= MM_CAMERA_MAX_NUM_SENSORS) {
        LOGE("Invalid camera id: %d", camera_id);
        return -EINVAL;
    }
    bool hasFlash = false;
    char flashNode[QCAMERA_MAX_FILEPATH_LENGTH];
    QCamera3HardwareInterface::getFlashInfo(camera_id, hasFlash, flashNode);
    if (!hasFlash) {
        return -ENOSYS;
    } else if (m_cameraOpen[camera_id]) {
        return -EBUSY;
    } else if (m_flashModeValid[camera_id] && mode == m_flashOn[camera_id]) {
        LOGD("flash %d is already in requested state: %d",
                camera_id,
                mode);
        retVal = -EALREADY;
    } else if (m_flashFds[camera_id] < 0) {
        LOGE("called for uninited flash: %d", camera_id);
        retVal = -EINVAL;
    }  else {
        memset(&cfg, 0, sizeof(struct msm_flash_cfg_data_t));
        for (int i = 0; i < MAX_LED_TRIGGERS; i++)
            cfg.flash_current[i] = QCAMERA_TORCH_CURRENT_VALUE;
        cfg.cfg_type = mode ? CFG_FLASH_LOW: CFG_FLASH_OFF;

        m_flashModeValid[camera_id] = false;
        retVal = ioctl(m_flashFds[camera_id],
                        VIDIOC_MSM_FLASH_CFG,
                        &cfg);
        if (retVal < 0) {
            retVal = -errno;
            LOGE("Unable to change flash mode to %d for camera id: %d",
                     mode, camera_id);
        } else
        {
            m_flashOn[camera_id] = mode;
            m_flashModeValid[camera_id] = true;
        }
    }
    return retVal;
}

/*===========================================================================
 * FUNCTION   : deinitFlash
 *
 * DESCRIPTION: Release the flash unit associated with a given camera
 *              position. This function is blocking until the operation
 *              completes or fails.
 *
 * PARAMETERS :
 *   @camera_id : Camera id of the flash.
 *
 * RETURN     :
 *   0        : success
 *   -EINVAL  : No camera present at camera_id.
 *==========================================================================*/
int32_t QCameraFlash::deinitFlashLocked(const int camera_id)
{
    int32_t retVal = 0;

    if (camera_id < 0 || camera_id >= MM_CAMERA_MAX_NUM_SENSORS) {
        LOGE("Invalid camera id: %d", camera_id);
        retVal = -EINVAL;
    } else if (m_flashFds[camera_id] < 0) {
        m_flashOn[camera_id] = false;
        m_flashModeValid[camera_id] = false;
    } else {
        setFlashModeLocked(camera_id, false);

        struct msm_flash_cfg_data_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.cfg_type = CFG_FLASH_RELEASE;
        m_flashModeValid[camera_id] = false;
        retVal = ioctl(m_flashFds[camera_id],
                VIDIOC_MSM_FLASH_CFG,
                &cfg);
        if (retVal < 0) {
            retVal = -errno;
            LOGE("Failed to release flash for camera id: %d",
                    camera_id);
            // Retain ownership so RELEASE can be retried.
            return retVal;
        }

        close(m_flashFds[camera_id]);
        m_flashFds[camera_id] = -1;
        m_flashOn[camera_id] = false;
        m_flashModeValid[camera_id] = false;
    }

    return retVal;
}

/*===========================================================================
 * FUNCTION   : reserveFlashForCamera
 *
 * DESCRIPTION: Give control of the flash to the camera, and notify
 *              framework that the flash has become unavailable.
 *
 * PARAMETERS :
 *   @camera_id : Camera id of the flash.
 *
 * RETURN     :
 *   0        : success
 *   -EINVAL  : No camera present at camera_id or not inited.
 *   -ENOSYS  : No callback available for torch_mode_status_change.
 *==========================================================================*/
int32_t QCameraFlash::reserveFlashForCamera(const int camera_id)
{
    int32_t retVal = 0;
    {
        android::Mutex::Autolock lock(m_lock);
        if (camera_id < 0 || camera_id >= MM_CAMERA_MAX_NUM_SENSORS) {
            LOGE("Invalid camera id: %d", camera_id);
            retVal = -EINVAL;
        } else if (m_cameraOpen[camera_id]) {
            LOGE("Flash already reserved for camera id: %d",
                    camera_id);
            retVal = -EBUSY;
        } else {
            bool hasFlash = false;
            char flashNode[QCAMERA_MAX_FILEPATH_LENGTH];

            QCamera3HardwareInterface::getFlashInfo(camera_id,
                    hasFlash,
                    flashNode);

            if (m_callbacks == NULL ||
                    m_callbacks->torch_mode_status_change == NULL) {
                LOGE("Callback is not defined!");
                retVal = -ENOSYS;
            } else if (hasFlash && flashNode[0] == '\0') {
                retVal = -ENODEV;
            } else if (hasFlash && cameraOwnerLocked(camera_id) >= 0) {
                retVal = -EBUSY;
            } else {
                int torchOwner = hasFlash ? torchOwnerLocked(camera_id) : -1;
                retVal = torchOwner >= 0 ? deinitFlashLocked(torchOwner) : 0;
                if (retVal == 0) {
                    m_cameraOpen[camera_id] = true;
                    if (hasFlash) {
                        queueGroupStatusLocked(camera_id, TORCH_MODE_STATUS_NOT_AVAILABLE);
                    }
                }
            }
        }
    }
    dispatchCallbacks();
    return retVal;
}

/*===========================================================================
 * FUNCTION   : releaseFlashFromCamera
 *
 * DESCRIPTION: Release control of the flash from the camera, and notify
 *              framework that the flash has become available.
 *
 * PARAMETERS :
 *   @camera_id : Camera id of the flash.
 *
 * RETURN     :
 *   0        : success
 *   -EINVAL  : No camera present at camera_id or not inited.
 *   -ENOSYS  : No callback available for torch_mode_status_change.
 *==========================================================================*/
int32_t QCameraFlash::releaseFlashFromCamera(const int camera_id)
{
    int32_t retVal = 0;
    {
        android::Mutex::Autolock lock(m_lock);
        if (camera_id < 0 || camera_id >= MM_CAMERA_MAX_NUM_SENSORS) {
            LOGE("Invalid camera id: %d", camera_id);
            retVal = -EINVAL;
        } else if (!m_cameraOpen[camera_id]) {
            LOGD("Flash not reserved for camera id: %d",
                    camera_id);
        } else {
            m_cameraOpen[camera_id] = false;

            bool hasFlash = false;
            char flashNode[QCAMERA_MAX_FILEPATH_LENGTH];

            QCamera3HardwareInterface::getFlashInfo(camera_id,
                    hasFlash,
                    flashNode);

            if (m_callbacks == NULL ||
                    m_callbacks->torch_mode_status_change == NULL) {
                LOGE("Callback is not defined!");
                retVal = -ENOSYS;
            } else if (!hasFlash) {
                LOGD("Suppressing callback "
                        "because no flash exists for camera id: %d",
                        camera_id);
            } else if (cameraOwnerLocked(camera_id) < 0) {
                queueGroupStatusLocked(camera_id, TORCH_MODE_STATUS_AVAILABLE_OFF);
            }
        }
    }
    dispatchCallbacks();
    return retVal;
}

}; // namespace qcamera
