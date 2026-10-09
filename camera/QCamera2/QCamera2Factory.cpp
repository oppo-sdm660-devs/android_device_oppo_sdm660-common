/* Copyright (c) 2012-2018, The Linux Foundation. All rights reserved.
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

#define LOG_TAG "QCamera2Factory"

// System dependencies
#include <stdlib.h>
#include <cmath>
#include <utils/Errors.h>
#include <cutils/properties.h>

// Camera dependencies
#ifdef QCAMERA_HAL1_SUPPORT
#include "hardware/camera.h"
#include "HAL/QCamera2HWI.h"
#include "QCameraMuxer.h"
#endif

#include "hardware/camera3.h"
#include "HAL3/QCamera3HWI.h"
#include "HAL3/QCamera3LogicalMultiCam.h"
#include "HAL3/QCamera3SatPolicy.h"
#include "HAL3/QCamera3VendorTags.h"
#include "util/QCameraFlash.h"
#include "QCamera2Factory.h"
#include "QCameraTrace.h"
extern "C" {
#include "mm_camera_dbg.h"
}

using namespace android;

namespace qcamera {

namespace {

// One enumerated camera's real geometry, decoded from the static metadata the
// HAL publishes for it: the decoded lens focal length, the real physical sensor
// size, the real pixel/active arrays and the decoded mount angle. The field of
// view is derived from the real focal length and the real sensor size, never
// from a fixed table. A record the HAL does not publish fails the decode so the
// caller can leave the stage's sentinel/refusal in place instead of inventing a
// value.
bool logicalPhysicalGeometry(const camera_info &info,
        QCamera3LogicalMultiCam::Physical &physical)
{
    const camera_metadata_t *metadata = info.static_camera_characteristics;
    if (metadata == NULL) return false;
    camera_metadata_ro_entry_t entry;
    if (find_camera_metadata_ro_entry(metadata,
            ANDROID_LENS_INFO_AVAILABLE_FOCAL_LENGTHS, &entry) != 0 ||
            entry.count == 0 || !std::isfinite(entry.data.f[0]) ||
            entry.data.f[0] <= 0.0f) return false;
    const double focal = entry.data.f[0];
    if (find_camera_metadata_ro_entry(metadata,
            ANDROID_SENSOR_INFO_PHYSICAL_SIZE, &entry) != 0 || entry.count != 2 ||
            !std::isfinite(entry.data.f[0]) || !std::isfinite(entry.data.f[1]) ||
            entry.data.f[0] <= 0.0f || entry.data.f[1] <= 0.0f) return false;
    const double sensorWidth = entry.data.f[0];
    const double sensorHeight = entry.data.f[1];
    if (find_camera_metadata_ro_entry(metadata,
            ANDROID_SENSOR_INFO_PIXEL_ARRAY_SIZE, &entry) != 0 || entry.count != 2 ||
            entry.data.i32[0] <= 0 || entry.data.i32[1] <= 0) return false;
    const int32_t pixelWidth = entry.data.i32[0];
    const int32_t pixelHeight = entry.data.i32[1];
    if (find_camera_metadata_ro_entry(metadata,
            ANDROID_SENSOR_INFO_ACTIVE_ARRAY_SIZE, &entry) != 0 || entry.count != 4 ||
            entry.data.i32[2] <= 0 || entry.data.i32[3] <= 0) return false;
    const int32_t activeWidth = entry.data.i32[2];
    const int32_t activeHeight = entry.data.i32[3];
    if (find_camera_metadata_ro_entry(metadata, ANDROID_SENSOR_ORIENTATION, &entry) != 0 ||
            entry.count != 1 || entry.data.i32[0] < 0 || entry.data.i32[0] >= 360)
        return false;
    const int32_t orientation = entry.data.i32[0];
    const double pi = 3.14159265358979323846;
    physical.focalLengthMm = float(focal);
    physical.horizontalFovDeg = float(2.0 * std::atan(sensorWidth / (2.0 * focal)) *
            180.0 / pi);
    physical.verticalFovDeg = float(2.0 * std::atan(sensorHeight / (2.0 * focal)) *
            180.0 / pi);
    physical.pixelWidth = uint32_t(pixelWidth);
    physical.pixelHeight = uint32_t(pixelHeight);
    physical.activeArrayWidth = uint32_t(activeWidth);
    physical.activeArrayHeight = uint32_t(activeHeight);
    physical.mountAngleDeg = orientation;
    return true;
}

} // namespace

QCamera2Factory *gQCamera2Factory = NULL;
pthread_mutex_t gCamLock = PTHREAD_MUTEX_INITIALIZER;
#ifdef QCAMERA_HAL1_SUPPORT
QCameraMuxer *gQCameraMuxer = NULL;
#endif

//Total number of cameras opened simultaneously.
//This variable updation is protected by gCamLock.
uint8_t gNumCameraSessions = 0;

volatile uint32_t gKpiDebugLevel = 1;

/*===========================================================================
 * FUNCTION   : QCamera2Factory
 *
 * DESCRIPTION: default constructor of QCamera2Factory
 *
 * PARAMETERS : none
 *
 * RETURN     : None
 *==========================================================================*/
QCamera2Factory::QCamera2Factory()
{
    // The platform registers its vendor tag descriptor only after the camera
    // module's get_number_of_cameras() has returned: the legacy HIDL provider
    // runs LegacyCameraProviderImpl_2_4::initialize() -> CameraModule::init()
    // (which creates this factory) and calls setUpVendorTags() only afterwards.
    // The dual / SAT / logical multi-camera publication probes below build
    // camera characteristics through QCamera3HardwareInterface::getCamInfo()
    // from this constructor, i.e. before that registration. With no vendor tag
    // ops registered, every vendor tag written by initStaticMetadata() is
    // silently dropped (libcamera_metadata cannot resolve the tag type) and the
    // affected cameras publish characteristics without any vendor key, which in
    // turn makes their own validateCaptureRequest() refuse every capture
    // request. Register the HAL's own table first; the provider's descriptor is
    // built from the same tables and replaces it afterwards.
    QCamera3VendorTags::registerWithMetadata();

    mHalDescriptors = NULL;
    mCallbacks = NULL;
    mNumOfCameras = get_num_of_cameras();
    mNumOfCameras_expose = get_num_of_cameras_to_expose();
#ifndef QCAMERA_HAL1_SUPPORT
    // Paired (dual) publication. The enumeration above yields the real sensors
    // only; the paired device that streams both rear sensors is appended by the
    // stack when the installed unit's own calibration and dual core are present,
    // so this is a capability probe, not a topology assumption.
    if (mNumOfCameras == 3 && mNumOfCameras_expose == 3) {
        if (!QCamera3HardwareInterface::dualSessionAvailable(0, 2)) {
            LOGI("dual camera not published: paired session unavailable "
                    "(unit dual calibration or dual core missing)");
        } else {
            const int32_t pairedRc = publish_hal3_dual_camera();
            if (pairedRc == 0) {
                // The publication appended exactly one device and already set
                // both stack counters to the new total
                // (stack/mm-camera-interface/src/mm_camera_interface.c:2840-2846);
                // its own precondition is num_cam_to_expose == num_cam
                // (:2807-2809). Re-running get_num_of_cameras() here would
                // memset g_cam_ctrl (:2634) and re-enumerate the media graph
                // (:2745-2776), dropping the device that was just published, so
                // read the updated expose count without discovery (:2792-2798).
                mNumOfCameras_expose = get_num_of_cameras_to_expose();
                mNumOfCameras = mNumOfCameras_expose;
                LOGI("dual camera published: cameras=%d expose=%d",
                        mNumOfCameras, mNumOfCameras_expose);
            } else {
                LOGI("dual camera not published: publish_hal3_dual_camera rc=%d",
                        pairedRc);
            }
        }
    }
#endif
    int bDualCamera = 0;
    char propDefault[PROPERTY_VALUE_MAX];
    char prop[PROPERTY_VALUE_MAX];
    property_get("persist.vendor.camera.HAL3.enabled", prop, "1");
    int isHAL3Enabled = atoi(prop);
#ifndef QCAMERA_HAL1_SUPPORT
    isHAL3Enabled = 1;
#endif

    // Signifies whether system has to enable dual camera mode
    snprintf(propDefault, PROPERTY_VALUE_MAX, "%d", isDualCamAvailable(isHAL3Enabled));
    property_get("persist.vendor.camera.dual.camera", prop, propDefault);
    bDualCamera = atoi(prop);
    LOGH("dualCamera:%d ", bDualCamera);
#ifndef QCAMERA_HAL1_SUPPORT
    bDualCamera = 0;
#endif

    if(bDualCamera) {
        LOGI("Enabling QCamera Muxer");
#ifdef QCAMERA_HAL1_SUPPORT
        if (!gQCameraMuxer) {
            QCameraMuxer::getCameraMuxer(&gQCameraMuxer, mNumOfCameras);
            if (!gQCameraMuxer) {
                LOGE("Error !! Failed to get QCameraMuxer");
            }
        }
#endif
    }
#ifdef QCAMERA_HAL1_SUPPORT
    if (!gQCameraMuxer && (mNumOfCameras > 0) &&(mNumOfCameras <= MM_CAMERA_MAX_NUM_SENSORS)) {
#else
    if ((mNumOfCameras > 0) &&(mNumOfCameras <= MM_CAMERA_MAX_NUM_SENSORS)) {
#endif
        mHalDescriptors = new hal_desc[mNumOfCameras];
        if ( NULL != mHalDescriptors) {
            uint32_t cameraId = 0;

            for (int i = 0; i < mNumOfCameras ; i++, cameraId++) {
                mHalDescriptors[i].cameraId = cameraId;
                // Set Device version to 3.x when both HAL3 is enabled & its BAYER sensor
#ifdef QCAMERA_HAL1_SUPPORT
                if (isHAL3Enabled && !(is_yuv_sensor(cameraId))) {
                    mHalDescriptors[i].device_version =
                            CAMERA_DEVICE_API_VERSION_3_0;
                } else {
                    mHalDescriptors[i].device_version =
                            CAMERA_DEVICE_API_VERSION_1_0;
                }
#else
                mHalDescriptors[i].device_version = CAMERA_DEVICE_API_VERSION_3_0;
#endif
            }
        } else {
            LOGE("Not enough resources to allocate HAL descriptor table!");
        }
    } else {
        LOGI("%d camera devices detected!", mNumOfCameras);
    }
#ifndef QCAMERA_HAL1_SUPPORT
    // Standard logical multi-camera surface (API 3.5) on top of the enumeration
    // and the paired publication above. The admission decision belongs to
    // QCamera3LogicalMultiCam and is fed only real device records; a refusal
    // leaves the camera list exactly as published, and a success appends one
    // extra public id without renumbering or hiding any standalone camera.
    // This must run after mHalDescriptors exists: the accepted plan is appended
    // to that table, so a later rebuild of the table would drop the mapping from
    // the appended public id to its backend camera.
    if (mNumOfCameras == 4 && mNumOfCameras_expose == 4)
        publishHal3LogicalCamera();
#endif
}

/*===========================================================================
 * FUNCTION   : ~QCamera2Factory
 *
 * DESCRIPTION: deconstructor of QCamera2Factory
 *
 * PARAMETERS : none
 *
 * RETURN     : None
 *==========================================================================*/
QCamera2Factory::~QCamera2Factory()
{
    if ( NULL != mHalDescriptors ) {
        delete [] mHalDescriptors;
    }
#ifdef QCAMERA_HAL1_SUPPORT
    if (gQCameraMuxer) {
        delete gQCameraMuxer;
        gQCameraMuxer = NULL;
    }
#endif
}

/*===========================================================================
 * FUNCTION   : publishHal3LogicalCamera
 *
 * DESCRIPTION: Admission and publication of the standard logical multi-camera
 *              surface. The decision is QCamera3LogicalMultiCam::publish(); the
 *              inputs are the real decoded records of the two enumerated
 *              physical cameras that form the paired session, the bound dual
 *              geometry and the declared zoom range. On refusal nothing is
 *              published and the standalone ids stay exactly as enumerated.
 *
 * PARAMETERS : none
 *
 * RETURN     : NO_ERROR when one logical public id was appended, negative errno
 *              otherwise (nothing changed in that case).
 *==========================================================================*/
int QCamera2Factory::publishHal3LogicalCamera()
{
    if (mHalDescriptors == NULL || mNumOfCameras != 4 || mNumOfCameras_expose != 4) {
        LOGI("logical multi-camera not published: cameras=%d expose=%d descriptors=%p",
                mNumOfCameras, mNumOfCameras_expose, mHalDescriptors);
        return -ENODEV;
    }
    // The two enumerated physical cameras the paired session is really bound to.
    const uint32_t wideId = 0;
    const uint32_t auxId = 2;
    if (!QCamera3HardwareInterface::dualSessionAvailable(wideId, auxId)) {
        LOGI("logical multi-camera not published: paired session unavailable");
        return -ENOTSUP;
    }
    camera_info infos[2] = {};
    if (QCamera3HardwareInterface::getCamInfo(wideId, &infos[0]) != NO_ERROR ||
            QCamera3HardwareInterface::getCamInfo(auxId, &infos[1]) != NO_ERROR) {
        LOGE("logical multi-camera not published: camera info unavailable");
        return -ENODEV;
    }
    QCamera3LogicalMultiCam::Input input = {};
    input.physicalCount = 2;
    input.physical[0].cameraId = wideId;
    input.physical[1].cameraId = auxId;
    for (size_t i = 0; i < 2; ++i) {
        if (!logicalPhysicalGeometry(infos[i], input.physical[i])) {
            LOGI("logical multi-camera not published: physical geometry of id %u "
                    "not decodable", input.physical[i].cameraId);
            return -ENODATA;
        }
        input.physical[i].standalonePublished = 1; // both stay enumerated on their own
        // The paired session is the real synchronization report of this unit;
        // the leader/follower hardware sync record itself is not decoded here.
        input.physical[i].syncCapable = 1;
    }
    // Roles come from the real decoded lenses: the shorter focal length and the
    // wider field of view is the wide camera. Two identical lenses get no role
    // at all, so publish() refuses instead of guessing a zoom pair.
    if (input.physical[0].focalLengthMm == input.physical[1].focalLengthMm) {
        input.physical[0].role = QCamera3LogicalMultiCam::ROLE_NONE;
        input.physical[1].role = QCamera3LogicalMultiCam::ROLE_NONE;
    } else if (input.physical[0].focalLengthMm < input.physical[1].focalLengthMm) {
        input.physical[0].role = QCamera3LogicalMultiCam::ROLE_WIDE;
        input.physical[1].role = QCamera3LogicalMultiCam::ROLE_TELE;
    } else {
        input.physical[0].role = QCamera3LogicalMultiCam::ROLE_TELE;
        input.physical[1].role = QCamera3LogicalMultiCam::ROLE_WIDE;
    }
    // A new public id after every existing one; never a standalone id.
    input.logicalId = uint32_t(mNumOfCameras_expose);
    input.logicalIdFree = 1;
    for (int i = 0; i < mNumOfCameras_expose; ++i) {
        if (mHalDescriptors[i].cameraId == input.logicalId) input.logicalIdFree = 0;
    }
    input.geometryBound =
            QCamera3HardwareInterface::dualGeometryAvailable(wideId, auxId) ? 1 : 0;
    input.concurrentStreams = 1; // guarded by dualSessionAvailable() above
    input.hardwareSync = 0;      // no verified leader/follower record in this tree
    float maxDigitalZoom = 0.0f;
    camera_metadata_ro_entry_t zoom;
    if (find_camera_metadata_ro_entry(infos[0].static_camera_characteristics,
            ANDROID_SCALER_AVAILABLE_MAX_DIGITAL_ZOOM, &zoom) == 0 && zoom.count == 1 &&
            std::isfinite(zoom.data.f[0])) {
        maxDigitalZoom = zoom.data.f[0];
    }
    input.maxDigitalZoom = maxDigitalZoom;
    input.zoomRatioMin = QCamera3LogicalMultiCam::kMinZoomRatio;
    input.zoomRatioMax = maxDigitalZoom;
    // The pair must also be a usable SAT (smooth-zoom) pair: QCamera3SatPolicy
    // derives the switch point from the two real decoded lenses and refuses a
    // pair whose optical separation is too small or whose FOV and focal ratios
    // disagree. No zoom-switch behaviour is implemented here; only the real
    // admission is required before a logical zoom device is published.
    QCamera3SatPolicy::Input sat = {};
    sat.wide.present = 1;
    sat.tele.present = 1;
    for (size_t i = 0; i < 2; ++i) {
        QCamera3SatPolicy::Camera &camera = input.physical[i].role ==
                QCamera3LogicalMultiCam::ROLE_WIDE ? sat.wide : sat.tele;
        camera.focalLengthMm = input.physical[i].focalLengthMm;
        camera.horizontalFovDeg = input.physical[i].horizontalFovDeg;
        camera.verticalFovDeg = input.physical[i].verticalFovDeg;
        camera.pixelWidth = input.physical[i].pixelWidth;
        camera.pixelHeight = input.physical[i].pixelHeight;
        // No decoded standby/low-power sensor mode exists in this tree, so the
        // parked-sensor mode is never admitted or applied.
        camera.lowPowerCapable = 0;
    }
    sat.calibrationBound = input.geometryBound;
    sat.concurrentStreams = input.concurrentStreams;
    sat.maxDigitalZoom = maxDigitalZoom;
    QCamera3SatPolicy::Thresholds satThresholds = {};
    if (!QCamera3SatPolicy::admitsSatMode(sat, satThresholds)) {
        LOGI("logical multi-camera refused: SAT pair not admitted (wide %.2fmm %.1fdeg, "
                "tele %.2fmm %.1fdeg, zoom %.1f)",
                sat.wide.focalLengthMm, sat.wide.horizontalFovDeg,
                sat.tele.focalLengthMm, sat.tele.horizontalFovDeg, maxDigitalZoom);
        return -ENOTSUP;
    }
    // Per-physical overridable request keys: the real request manifest of the
    // paired daemon camera that will serve the logical session, restricted to
    // the per-sensor controls the per-camera parameter translation really
    // applies (the HWI consumes exactly those in process_capture_request). When
    // none can be honoured the list stays empty and publish() refuses instead of
    // advertising a logical device whose physical requests could not be served.
    input.requestKeyCount = 0;
    input.sessionKeyCount = 0; // this HAL publishes no session parameters
    {
        const uint32_t pairedId = uint32_t(mNumOfCameras - 1);
        camera_info pairedInfo = {};
        if (QCamera3HardwareInterface::getCamInfo(pairedId, &pairedInfo) == NO_ERROR)
            input.requestKeyCount = QCamera3HardwareInterface::logicalPhysicalRequestKeys(
                    pairedId, input.requestKeys, QCamera3LogicalMultiCam::kMaxKeys);
    }
    QCamera3LogicalMultiCam::Plan plan = {};
    const int32_t status = QCamera3LogicalMultiCam::publish(input, plan);
    if (status != NO_ERROR || !plan.publish) {
        LOGI("logical multi-camera refused: reason=%s status=%d",
                QCamera3LogicalMultiCam::reasonString(plan.reason), plan.status);
        return status < 0 ? status : -ENOTSUP;
    }
    if (!QCamera3LogicalMultiCam::keepsStandaloneVisible(input, plan)) {
        LOGE("logical multi-camera not published: standalone visibility invariant");
        return -EINVAL;
    }
    // Hand the accepted plan to the HWI before the framework enumerates the new
    // id: the paired camera's characteristics are rebuilt with the standard
    // logical surface on the next get_camera_info().
    const uint32_t backendId = uint32_t(mNumOfCameras - 1);
    const int32_t installRc =
            QCamera3HardwareInterface::installLogicalMultiCamPlan(plan, backendId);
    if (installRc != NO_ERROR) {
        LOGE("logical multi-camera not published: plan install rc=%d", installRc);
        return installRc;
    }
    // The logical id is appended as one extra public id mapped to the paired
    // daemon camera published above. Existing ids keep their exact meaning.
    hal_desc *grown = new hal_desc[mNumOfCameras_expose + 1];
    if (grown == NULL) return -ENOMEM;
    for (int i = 0; i < mNumOfCameras_expose; ++i) grown[i] = mHalDescriptors[i];
    grown[mNumOfCameras_expose].cameraId = backendId;
    grown[mNumOfCameras_expose].device_version = CAMERA_DEVICE_API_VERSION_3_0;
    delete [] mHalDescriptors;
    mHalDescriptors = grown;
    ++mNumOfCameras;
    ++mNumOfCameras_expose;
    LOGI("logical multi-camera published id=%u physical=%u,%u sync=%d switch=%.3f keys=%u",
            plan.logicalId, plan.physicalIds[0], plan.physicalIds[1], plan.syncType,
            satThresholds.switchOver, unsigned(plan.requestKeyCount));
    return NO_ERROR;
}

/*===========================================================================
 * FUNCTION   : get_number_of_cameras
 *
 * DESCRIPTION: static function to query number of cameras detected
 *
 * PARAMETERS : none
 *
 * RETURN     : number of cameras detected
 *==========================================================================*/
int QCamera2Factory::get_number_of_cameras()
{
    int numCameras = 0;

    if (!gQCamera2Factory) {
        gQCamera2Factory = new QCamera2Factory();
        if (!gQCamera2Factory) {
            LOGE("Failed to allocate Camera2Factory object");
            return 0;
        }
    }
#ifdef QCAMERA_HAL1_SUPPORT
    if(gQCameraMuxer)
        numCameras = gQCameraMuxer->get_number_of_cameras();
    else
#endif
        numCameras = gQCamera2Factory->getNumberOfCameras();

    LOGH("num of cameras: %d", numCameras);
    return numCameras;
}

/*===========================================================================
 * FUNCTION   : get_camera_info
 *
 * DESCRIPTION: static function to query camera information with its ID
 *
 * PARAMETERS :
 *   @camera_id : camera ID
 *   @info      : ptr to camera info struct
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int QCamera2Factory::get_camera_info(int camera_id, struct camera_info *info)
{
    int rc = NO_ERROR;
#ifdef QCAMERA_HAL1_SUPPORT
    if(gQCameraMuxer)
        rc = gQCameraMuxer->get_camera_info(camera_id, info);
    else
#endif
        rc =  gQCamera2Factory->getCameraInfo(camera_id, info);

    return rc;
}

/*===========================================================================
 * FUNCTION   : set_callbacks
 *
 * DESCRIPTION: static function to set callbacks function to camera module
 *
 * PARAMETERS :
 *   @callbacks : ptr to callback functions
 *
 * RETURN     : NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int QCamera2Factory::set_callbacks(const camera_module_callbacks_t *callbacks)
{
    int rc = NO_ERROR;
#ifdef QCAMERA_HAL1_SUPPORT
    if(gQCameraMuxer)
        rc = gQCameraMuxer->set_callbacks(callbacks);
    else
#endif
        rc =  gQCamera2Factory->setCallbacks(callbacks);

    return rc;
}

/*===========================================================================
 * FUNCTION   : open_legacy
 *
 * DESCRIPTION: Function to open older hal version implementation
 *
 * PARAMETERS :
 *   @hw_device : ptr to struct storing camera hardware device info
 *   @camera_id : camera ID
 *   @halVersion: Based on camera_module_t.common.module_api_version
 *
 * RETURN     : 0  -- success
 *              none-zero failure code
 *==========================================================================*/
int QCamera2Factory::open_legacy(const struct hw_module_t* module,
            const char* id, uint32_t halVersion, struct hw_device_t** device)
{
    int rc = NO_ERROR;
    if (module != &HAL_MODULE_INFO_SYM.common) {
        LOGE("Invalid module. Trying to open %p, expect %p",
            module, &HAL_MODULE_INFO_SYM.common);
        return INVALID_OPERATION;
    }
    if (!id) {
        LOGE("Invalid camera id");
        return BAD_VALUE;
    }
#ifdef QCAMERA_HAL1_SUPPORT
    if(gQCameraMuxer)
        rc =  gQCameraMuxer->open_legacy(module, id, halVersion, device);
    else
#endif
        rc =  gQCamera2Factory->openLegacy(atoi(id), halVersion, device);

    return rc;
}

/*===========================================================================
 * FUNCTION   : set_torch_mode
 *
 * DESCRIPTION: Attempt to turn on or off the torch mode of the flash unit.
 *
 * PARAMETERS :
 *   @camera_id : camera ID
 *   @on        : Indicates whether to turn the flash on or off
 *
 * RETURN     : 0  -- success
 *              none-zero failure code
 *==========================================================================*/
int QCamera2Factory::set_torch_mode(const char* camera_id, bool on)
{
    return gQCamera2Factory->setTorchMode(camera_id, on);
}

/*===========================================================================
 * FUNCTION   : getNumberOfCameras
 *
 * DESCRIPTION: query number of cameras detected
 *
 * PARAMETERS : none
 *
 * RETURN     : number of cameras detected
 *==========================================================================*/
int QCamera2Factory::getNumberOfCameras()
{
    return mNumOfCameras_expose;
}

/*===========================================================================
 * FUNCTION   : getCameraInfo
 *
 * DESCRIPTION: query camera information with its ID
 *
 * PARAMETERS :
 *   @camera_id : camera ID
 *   @info      : ptr to camera info struct
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int QCamera2Factory::getCameraInfo(int camera_id, struct camera_info *info)
{
    int rc;

    if (!mNumOfCameras || camera_id >= mNumOfCameras || !info ||
        (camera_id < 0)) {
        LOGE("Error getting camera info!! mNumOfCameras = %d,"
                "camera_id = %d, info = %p",
                 mNumOfCameras, camera_id, info);
        return -ENODEV;
    }

    if ( NULL == mHalDescriptors ) {
        LOGE("Hal descriptor table is not initialized!");
        return NO_INIT;
    }

    LOGI("Camera id %d API version %d",
            camera_id, mHalDescriptors[camera_id].device_version);

    // Need ANDROID_FLASH_INFO_AVAILABLE property for flashlight widget to
    // work and so get the static data regardless of HAL version
    rc = QCamera3HardwareInterface::getCamInfo(
            mHalDescriptors[camera_id].cameraId, info);
    if (mHalDescriptors[camera_id].device_version ==
            CAMERA_DEVICE_API_VERSION_1_0) {
        info->device_version = CAMERA_DEVICE_API_VERSION_1_0;
    }
    return rc;
}

/*===========================================================================
 * FUNCTION   : setCallbacks
 *
 * DESCRIPTION: set callback functions to send asynchronous notifications to
 *              frameworks.
 *
 * PARAMETERS :
 *   @callbacks : callback function pointer
 *
 * RETURN     :
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int QCamera2Factory::setCallbacks(const camera_module_callbacks_t *callbacks)
{
    int rc = NO_ERROR;
    mCallbacks = callbacks;

    rc = QCameraFlash::getInstance().registerCallbacks(callbacks);
    if (rc != 0) {
        LOGE("Failed to register callbacks with flash module!");
    }

    return rc;
}

/*===========================================================================
 * FUNCTION   : cameraDeviceOpen
 *
 * DESCRIPTION: open a camera device with its ID
 *
 * PARAMETERS :
 *   @camera_id : camera ID
 *   @hw_device : ptr to struct storing camera hardware device info
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int QCamera2Factory::cameraDeviceOpen(int camera_id,
                    struct hw_device_t **hw_device)
{
    int rc = NO_ERROR;
    if (camera_id < 0 || camera_id >= mNumOfCameras)
        return -ENODEV;

    if ( NULL == mHalDescriptors ) {
        LOGE("Hal descriptor table is not initialized!");
        return NO_INIT;
    }

    LOGI("Open camera id %d API version %d",
            camera_id, mHalDescriptors[camera_id].device_version);

    if ( mHalDescriptors[camera_id].device_version == CAMERA_DEVICE_API_VERSION_3_0 ) {
        CAMSCOPE_INIT(CAMSCOPE_SECTION_HAL);
        QCamera3HardwareInterface *hw = new QCamera3HardwareInterface(mHalDescriptors[camera_id].cameraId,
                mCallbacks);
        if (!hw) {
            LOGE("Allocation of hardware interface failed");
            return NO_MEMORY;
        }
        rc = hw->openCamera(hw_device);
        if (rc != 0) {
            delete hw;
        }
    }
#ifdef QCAMERA_HAL1_SUPPORT
    else if (mHalDescriptors[camera_id].device_version == CAMERA_DEVICE_API_VERSION_1_0) {
        QCamera2HardwareInterface *hw = new QCamera2HardwareInterface((uint32_t)camera_id);
        if (!hw) {
            LOGE("Allocation of hardware interface failed");
            return NO_MEMORY;
        }
        rc = hw->openCamera(hw_device);
        if (rc != NO_ERROR) {
            delete hw;
        }
    }
#endif
    else {
        LOGE("Device version for camera id %d invalid %d",
              camera_id,
              mHalDescriptors[camera_id].device_version);
        return BAD_VALUE;
    }

    return rc;
}

/*===========================================================================
 * FUNCTION   : camera_device_open
 *
 * DESCRIPTION: static function to open a camera device by its ID
 *
 * PARAMETERS :
 *   @camera_id : camera ID
 *   @hw_device : ptr to struct storing camera hardware device info
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int QCamera2Factory::camera_device_open(
    const struct hw_module_t *module, const char *id,
    struct hw_device_t **hw_device)
{
    int rc = NO_ERROR;
    if (module != &HAL_MODULE_INFO_SYM.common) {
        LOGE("Invalid module. Trying to open %p, expect %p",
            module, &HAL_MODULE_INFO_SYM.common);
        return INVALID_OPERATION;
    }
    if (!id) {
        LOGE("Invalid camera id");
        return BAD_VALUE;
    }
#ifdef QCAMERA_HAL1_SUPPORT
    if(gQCameraMuxer)
        rc =  gQCameraMuxer->camera_device_open(module, id, hw_device);
    else
#endif
        rc = gQCamera2Factory->cameraDeviceOpen(atoi(id), hw_device);
    return rc;
}

struct hw_module_methods_t QCamera2Factory::mModuleMethods = {
    .open = QCamera2Factory::camera_device_open,
};

/*===========================================================================
 * FUNCTION   : openLegacy
 *
 * DESCRIPTION: Function to open older hal version implementation
 *
 * PARAMETERS :
 *   @camera_id : camera ID
 *   @halVersion: Based on camera_module_t.common.module_api_version
 *   @hw_device : ptr to struct storing camera hardware device info
 *
 * RETURN     : 0  -- success
 *              none-zero failure code
 *==========================================================================*/
int QCamera2Factory::openLegacy(
        int32_t cameraId, uint32_t halVersion, struct hw_device_t** hw_device)
{
#ifndef QCAMERA_HAL1_SUPPORT
    (void)hw_device;
#endif
    int rc = NO_ERROR;

    LOGI("openLegacy halVersion: %d cameraId = %d", halVersion, cameraId);
    //Assumption: all cameras can support legacy API version
    if (cameraId < 0 || cameraId >= gQCamera2Factory->getNumberOfCameras())
        return -ENODEV;

    switch(halVersion)
    {
#ifdef QCAMERA_HAL1_SUPPORT
        case CAMERA_DEVICE_API_VERSION_1_0:
        {
            CAMSCOPE_INIT(CAMSCOPE_SECTION_HAL);
            QCamera2HardwareInterface *hw =
                new QCamera2HardwareInterface((uint32_t)cameraId);
            if (!hw) {
                LOGE("Allocation of hardware interface failed");
                return NO_MEMORY;
            }
            rc = hw->openCamera(hw_device);
            if (rc != NO_ERROR) {
                delete hw;
            }
            break;
        }
#endif
        default:
            LOGE("Device API version: %d for camera id %d invalid",
                 halVersion, cameraId);
            return BAD_VALUE;
    }

    return rc;
}

/*===========================================================================
 * FUNCTION   : setTorchMode
 *
 * DESCRIPTION: Attempt to turn on or off the torch mode of the flash unit.
 *
 * PARAMETERS :
 *   @camera_id : camera ID
 *   @on        : Indicates whether to turn the flash on or off
 *
 * RETURN     : 0  -- success
 *              none-zero failure code
 *==========================================================================*/
int QCamera2Factory::setTorchMode(const char* camera_id, bool on)
{
    int retVal(0);
    long cameraIdLong(-1);
    char* endPointer = NULL;
    errno = 0;
    QCameraFlash& flash = QCameraFlash::getInstance();

    if (camera_id == NULL) {
        return -EINVAL;
    }
    cameraIdLong = strtol(camera_id, &endPointer, 10);

    if ((errno == ERANGE) ||
            (cameraIdLong < 0) ||
            (cameraIdLong >= static_cast<long>(get_number_of_cameras())) ||
            (endPointer == camera_id) ||
            (*endPointer != '\0')) {
        retVal = -EINVAL;
    } else {
        retVal = flash.setTorchMode(static_cast<int>(cameraIdLong), on);
    }

    return retVal;
}

/*===========================================================================
 * FUNCTION   : isDualCamAvailable
 *
 * DESCRIPTION: Function to check whether we have dual Camera HW available
 *
 * PARAMETERS :
 *   @hal3Enabled : HAL3 enable flag
 *
 * RETURN     : bool - true : have Dual Camera HW available
 *                           false : not have Dual Camera HW available
 *==========================================================================*/
bool QCamera2Factory::isDualCamAvailable(int hal3Enabled)
{
#ifdef QCAMERA_HAL1_SUPPORT
    bool rc = false;
    int i = 0;
    camera_info info;
    cam_sync_type_t cam_type = CAM_TYPE_MAIN;

    for (i = 0; i < mNumOfCameras; i++) {
        if (!hal3Enabled) {
            QCamera2HardwareInterface::getCapabilities(i, &info, &cam_type);
        }

        if(cam_type == CAM_TYPE_AUX) {
            LOGH("Have Dual Camera HW Avaiable.");
            rc = true;
            break;
        }
    }
    return rc;
#else
    (void)hal3Enabled;
    return false;
#endif
}

}; // namespace qcamera
