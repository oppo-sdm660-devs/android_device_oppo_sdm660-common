/* Copyright (c) 2014-2016,2019 The Linux Foundation. All rights reserved.
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

#define LOG_TAG "QCamera3VendorTags"

// Camera dependencies
#include "QCamera3HWI.h"
#include "QCamera3VendorTags.h"

extern "C" {
#include "mm_camera_dbg.h"
}

using namespace android;

namespace qcamera {

enum qcamera3_ext_tags qcamera3_ext3_section_bounds[QCAMERA3_SECTIONS_END -
    VENDOR_SECTION] = {
        QCAMERA3_PRIVATEDATA_END,
        QCAMERA3_CDS_END,
        QCAMERA3_CROP_END,
        QCAMERA3_TEMPORAL_DENOISE_END,
        QCAMERA3_ISO_EXP_PRIORITY_END,
        QCAMERA3_SATURATION_END,
        QCAMERA3_EXPOSURE_METER_END,
        QCAMERA3_AV_TIMER_END,
        QCAMERA3_DUALCAM_LINK_META_DATA_END,
        QCAMERA3_HAL_PRIVATEDATA_END,
        QCAMERA3_JPEG_ENCODE_CROP_END,
        QCAMERA3_VIDEO_HDR_END,
        QCAMERA3_IR_END,
        QCAMERA3_AEC_CONVERGENCE_SPEED_END,
        QCAMERA3_AWB_CONVERGENCE_SPEED_END,
        QCAMERA3_INSTANT_AEC_END,
        QCAMERA3_SHARPNESS_END,
        QCAMERA3_HISTOGRAM_END,
        QCAMERA3_BINNING_CORRECTION_END,
        QCAMERA3_STATS_END,
        QCAMERA3_SIMULTANEOUS_CAMERA_END,
        QCAMERA3_QUADRA_CFA_DATA_END,
        QCAMERA3_BOKEH_END,
        QCAMERA3_LOGICAL_CAM_END,
        QCAMERA3_MANUAL_WB_END,
        QCAMERA3_SAT_MODE_END,
        QCAMERA3_EIS_FLUSH_END,
        QCAMERA3_SWMFNR_END,
        QCAMERA3_NATIVE_ALGORITHMS_END,
        QCAMERA3_POSTPROCESS_END,
        QCAMERA3_STREAM_END,
        QCAMERA3_DIAGNOSTICS_END,
        QCAMERA3_AR_END,
};

typedef struct vendor_tag_info {
    const char *tag_name;
    uint8_t     tag_type;
} vendor_tag_info_t;

const char *qcamera3_ext_section_names[QCAMERA3_SECTIONS_END -
        VENDOR_SECTION] = {
    "org.codeaurora.qcamera3.privatedata",
    "org.codeaurora.qcamera3.CDS",
    "org.codeaurora.qcamera3.crop",
    "org.codeaurora.qcamera3.temporal_denoise",
    "org.codeaurora.qcamera3.iso_exp_priority",
    "org.codeaurora.qcamera3.saturation",
    "org.codeaurora.qcamera3.exposure_metering",
    "org.codeaurora.qcamera3.av_timer",
    "org.codeaurora.qcamera3.dualcam_link_meta_data",
    "org.codeaurora.qcamera3.hal_private_data",
    "org.codeaurora.qcamera3.jpeg_encode_crop",
    "org.codeaurora.qcamera3.video_hdr_mode",
    "org.codeaurora.qcamera3.ir",
    "org.codeaurora.qcamera3.aec_convergence_speed",
    "org.codeaurora.qcamera3.awb_convergence_speed",
    "org.codeaurora.qcamera3.instant_aec",
    "org.codeaurora.qcamera3.sharpness",
    "org.codeaurora.qcamera3.histogram",
    "org.codeaurora.qcamera3.binning_correction",
    "org.codeaurora.qcamera3.stats",
    "org.codeaurora.qcamera3.simultaneous_camera",
    "org.codeaurora.qcamera3.quadra_cfa",
    "org.codeaurora.qcamera3.bokeh",
    "org.codeaurora.qcamera3.logical",
    "org.codeaurora.qcamera3.manualWB",
    "org.codeaurora.qcamera3.sat",
    "org.quic.camera.recording",
    "org.codeaurora.qcamera3.swmfnr",
    "org.lineageos.camera.algorithms",
    "org.lineageos.camera.postprocess",
    "org.lineageos.camera.stream",
    "org.lineageos.camera.diagnostics",
    "org.lineageos.camera.ar"
};

vendor_tag_info_t qcamera3_privatedata[QCAMERA3_PRIVATEDATA_END - QCAMERA3_PRIVATEDATA_START] = {
    { "privatedata_reprocess", TYPE_INT32 }
};

vendor_tag_info_t qcamera3_cds[QCAMERA3_CDS_END - QCAMERA3_CDS_START] = {
    { "cds_mode", TYPE_INT32 },
    { "cds_info", TYPE_BYTE }
};

vendor_tag_info_t qcamera3_crop[QCAMERA3_CROP_END- QCAMERA3_CROP_START] = {
    { "count", TYPE_INT32 },
    { "data", TYPE_INT32},
    { "roimap", TYPE_INT32 }
};

vendor_tag_info_t qcamera3_temporal_denoise[QCAMERA3_TEMPORAL_DENOISE_END -
        QCAMERA3_TEMPORAL_DENOISE_START] = {
    { "enable", TYPE_BYTE },
    { "process_type", TYPE_INT32 }
};

vendor_tag_info qcamera3_iso_exp_priority[QCAMERA3_ISO_EXP_PRIORITY_END -
                                  QCAMERA3_ISO_EXP_PRIORITY_START] = {
    { "use_iso_exp_priority", TYPE_INT64 },
    { "select_priority", TYPE_INT32 },
    { "iso_available_modes", TYPE_INT32 },
    { "exposure_time_range", TYPE_INT64 }
};

vendor_tag_info qcamera3_saturation[QCAMERA3_SATURATION_END -
                                  QCAMERA3_SATURATION_START] = {
    { "use_saturation", TYPE_INT32 },
    { "range", TYPE_INT32 }
};

vendor_tag_info qcamera3_exposure_metering[QCAMERA3_EXPOSURE_METER_END -
                                  QCAMERA3_EXPOSURE_METER_START] = {
    { "exposure_metering_mode", TYPE_INT32},
    { "available_modes", TYPE_INT32 }
};

vendor_tag_info qcamera3_av_timer[QCAMERA3_AV_TIMER_END -
                                  QCAMERA3_AV_TIMER_START] = {
   {"use_av_timer", TYPE_BYTE }
};

vendor_tag_info_t
        qcamera3_dualcam_link_meta_data[QCAMERA3_DUALCAM_LINK_META_DATA_END -
        QCAMERA3_DUALCAM_LINK_META_DATA_START] = {
    { "enable",            TYPE_BYTE },
    { "is_main",           TYPE_BYTE },
    { "related_camera_id", TYPE_INT32 }
};

vendor_tag_info_t
        qcamera3_hal_privatedata[QCAMERA3_HAL_PRIVATEDATA_END -
        QCAMERA3_HAL_PRIVATEDATA_START] = {
    { "reprocess_flags",      TYPE_BYTE },
    { "reprocess_data_blob",  TYPE_BYTE },
    { "exif_debug_data_blob", TYPE_BYTE }
};

vendor_tag_info_t
        qcamera3_jpep_encode_crop[QCAMERA3_JPEG_ENCODE_CROP_END -
        QCAMERA3_JPEG_ENCODE_CROP_START] = {
    { "enable", TYPE_BYTE },
    { "rect",   TYPE_INT32 },
    { "roi",    TYPE_INT32}
};

vendor_tag_info_t qcamera3_video_hdr[QCAMERA3_VIDEO_HDR_END -
        QCAMERA3_VIDEO_HDR_START] = {
    { "vhdr_mode", TYPE_INT32 },
    { "vhdr_supported_modes", TYPE_INT32 }
};

vendor_tag_info_t qcamera3_ir[QCAMERA3_IR_END -
        QCAMERA3_IR_START] = {
    { "ir_mode", TYPE_INT32 },
    { "ir_supported_modes", TYPE_INT32}
};

vendor_tag_info_t qcamera3_aec_speed[QCAMERA3_AEC_CONVERGENCE_SPEED_END -
        QCAMERA3_AEC_CONVERGENCE_SPEED_START] = {
    {"aec_speed", TYPE_FLOAT }
};

vendor_tag_info_t qcamera3_awb_speed[QCAMERA3_AWB_CONVERGENCE_SPEED_END -
        QCAMERA3_AWB_CONVERGENCE_SPEED_START] = {
    {"awb_speed", TYPE_FLOAT }
};

vendor_tag_info_t
        qcamera3_instant_aec[QCAMERA3_INSTANT_AEC_END -
        QCAMERA3_INSTANT_AEC_START] = {
    { "instant_aec_mode", TYPE_INT32 },
    { "instant_aec_available_modes",   TYPE_INT32 }
};

vendor_tag_info_t qcamera3_sharpness[QCAMERA3_SHARPNESS_END -
        QCAMERA3_SHARPNESS_START] = {
    {"strength", TYPE_INT32 },
    {"range", TYPE_INT32 }
};

vendor_tag_info_t qcamera3_histogram[QCAMERA3_HISTOGRAM_END -
        QCAMERA3_HISTOGRAM_START] = {
    { "enable", TYPE_BYTE },
    { "buckets", TYPE_INT32 },
    { "max_count", TYPE_INT32 },
    { "stats", TYPE_INT32 }
};

vendor_tag_info_t qcamera3_binning_correction[QCAMERA3_BINNING_CORRECTION_END -
        QCAMERA3_BINNING_CORRECTION_START] = {
    { "binning_correction_mode", TYPE_INT32 },
    { "binning_correction_available_modes",   TYPE_INT32 }
};

vendor_tag_info_t qcamera3_stats[QCAMERA3_STATS_END -
        QCAMERA3_STATS_START] = {
    { "is_hdr_scene", TYPE_BYTE },
    { "is_hdr_scene_values", TYPE_BYTE },
    { "is_hdr_scene_confidence",   TYPE_FLOAT },
    { "is_hdr_scene_confidence_range", TYPE_FLOAT },
    { "bsgc_available", TYPE_BYTE },
    { "blink_detected", TYPE_BYTE },
    { "blink_degree", TYPE_BYTE },
    { "smile_degree", TYPE_BYTE },
    { "smile_confidence", TYPE_BYTE },
    { "gaze_angle", TYPE_BYTE },
    { "gaze_direction", TYPE_INT32 },
    { "gaze_degree", TYPE_BYTE }
};

vendor_tag_info_t qcamera3_simultaneous_camera[QCAMERA3_SIMULTANEOUS_CAMERA_END-
        QCAMERA3_SIMULTANEOUS_CAMERA_START] = {
    { "vfe1_reserved_rdi", TYPE_INT32 },
};

vendor_tag_info_t qcamera3_quadra_cfa[QCAMERA3_QUADRA_CFA_DATA_END -
        QCAMERA3_QUADRA_CFA_DATA_START] = {
    { "is_qcfa_sensor", TYPE_BYTE },
    { "qcfa_dimension", TYPE_INT32 },
};

vendor_tag_info_t qcamera3_bokeh[QCAMERA3_BOKEH_END -
        QCAMERA3_BOKEH_START] = {
    { "enable", TYPE_BYTE },
    { "blurLevel", TYPE_INT32 },
    { "status", TYPE_INT32 }
};

vendor_tag_info_t qcamera3_logical_cam[QCAMERA3_LOGICAL_CAM_END -
        QCAMERA3_LOGICAL_CAM_START] = {
    { "mode", TYPE_BYTE }
};

vendor_tag_info qcamera3_manual_wb[QCAMERA3_MANUAL_WB_END -
                                  QCAMERA3_MANUAL_WB_START] = {
    { "partial_mwb_mode", TYPE_INT32 },
    { "color_temperature_range", TYPE_INT32 },
    { "gains_range", TYPE_FLOAT },
    { "color_temperature", TYPE_INT32 },
    { "gains", TYPE_FLOAT }
};

vendor_tag_info qcamera3_swmfnr[QCAMERA3_SWMFNR_END -
                                  QCAMERA3_SWMFNR_START] = {
    { "enable", TYPE_BYTE }
};

vendor_tag_info_t qcamera3_native_algorithms[QCAMERA3_NATIVE_ALGORITHMS_END -
        QCAMERA3_NATIVE_ALGORITHMS_START] = {
    { "stillMode", TYPE_INT32 },
    { "beautyStrength", TYPE_INT32 },
    { "availableModes", TYPE_INT32 },
    { "filterType", TYPE_INT32 },
    { "filterFlipMode", TYPE_INT32 },
    { "availableFilterTypes", TYPE_INT32 },
    { "continuousEnabled", TYPE_INT32 },
    { "continuousBeautyStrength", TYPE_INT32 },
    { "continuousFilterType", TYPE_INT32 },
    { "continuousToneStrength", TYPE_INT32 },
    { "continuousAvailableEffects", TYPE_INT32 },
    { "continuousMaxPixels", TYPE_INT32 },
    { "sceneStrength", TYPE_INT32 },
    { "continuousSceneStrength", TYPE_INT32 },
    { "continuousSceneResult", TYPE_INT32 },
    { "relightStrength", TYPE_INT32 },
    { "relightAzimuth", TYPE_INT32 },
    { "relightElevation", TYPE_INT32 },
    { "frontBeautyStrength", TYPE_INT32 },
    { "frontBeautyFeatures", TYPE_INT32 },
    { "picBestFrames", TYPE_INT32 },
    { "picBestStrength", TYPE_INT32 },
    { "picBestOpenEyeOnly", TYPE_INT32 },
    { "blurlessThreshold", TYPE_INT32 },
    { "faceInfoMode", TYPE_INT32 },
    { "imageAdjust", TYPE_INT32 },
    { "singleBokehStrength", TYPE_INT32 },
    { "singleBokehSpotMode", TYPE_INT32 },
    { "singleBokehSpot", TYPE_FLOAT },
    { "portraitLightMode", TYPE_INT32 },
    { "portraitLightStrength", TYPE_INT32 },
    { "docMode", TYPE_INT32 },
    { "docStrength", TYPE_INT32 },
    { "autoHdrEnable", TYPE_INT32 },
    { "autoNightEnable", TYPE_INT32 },
    { "doubleExposureMode", TYPE_INT32 },
    { "doubleExposureStrength", TYPE_INT32 },
    { "doubleExposureTimeNs", TYPE_INT64 },
    { "longExposureEnable", TYPE_INT32 },
    { "remosaicEnable", TYPE_INT32 },
    { "exifComment", TYPE_BYTE },
    { "watermarkAsset", TYPE_BYTE },
    { "stickerAsset", TYPE_BYTE },
    { "stickerRect", TYPE_FLOAT },
    { "mirrorPreview", TYPE_INT32 },
    { "appPolicyMode", TYPE_INT32 },
    { "appMask", TYPE_INT32 },
    { "facePreviewParam", TYPE_INT32 },
    { "depthQuality", TYPE_INT32 },
    { "availableDocModes", TYPE_INT32 },
    { "maxLongExposureNs", TYPE_INT64 },
    { "longExposureRangeNs", TYPE_INT64 },
    { "availableRemosaicSizes", TYPE_INT32 },
    { "remosaicSupported", TYPE_BYTE },
    { "depthSupported", TYPE_BYTE },
    { "arTrackingSupported", TYPE_BYTE },
    { "professionalVer", TYPE_INT32 },
    { "hlgVideoSupported", TYPE_BYTE },
    { "nativeTrace", TYPE_BYTE },
    { "faceAttributes", TYPE_INT32 },
    { "estimatedSnapshotExpTime", TYPE_INT64 },
    { "hotPixelMapAvailable", TYPE_BYTE },
    { "appliedNativeMode", TYPE_INT32 },
    { "previewBokehStrength", TYPE_INT32 },
    { "previewBokehTemporalSmoothing", TYPE_INT32 },
    { "autoCaptureReason", TYPE_INT32 },
};

vendor_tag_info_t qcamera3_postprocess[QCAMERA3_POSTPROCESS_END -
        QCAMERA3_POSTPROCESS_START] = {
    { "settings", TYPE_INT32 },
    { "warpRegions", TYPE_FLOAT },
    { "overlay", TYPE_INT32 },
    { "watermark", TYPE_BYTE },
    { "faceTracking", TYPE_INT32 },
    { "watermarkLayout", TYPE_INT32 },
    { "mirror", TYPE_INT32 },
    { "exifUserComment", TYPE_BYTE },
};

// ORDER IS THE ABI: entry i is the name/type of QCAMERA3_STREAM_START + i, so this
// list must stay in the exact order of the enum in QCamera3VendorTags.h. Seven
// entries were rotated here (vstab* last) while the enum had them in the middle,
// so `lowPowerMode` resolved to the id of vstabOffset, the byte[] physical id was
// written into a FLOAT-registered tag, and every by-name consumer saw the wrong
// tag. Enforced by device-tree-nonsource-20261017/camera/tests/test_vendor_tag_tables.py.
vendor_tag_info_t qcamera3_stream[QCAMERA3_STREAM_END -
        QCAMERA3_STREAM_START] = {
    { "slowMotionRatio", TYPE_INT32 },
    { "availableSlowMotionRatios", TYPE_INT32 },
    { "videoHdrProfile", TYPE_INT32 },
    { "vstabCropFactor", TYPE_FLOAT },
    { "vstabOffset", TYPE_FLOAT },
    { "logicalZoomHint", TYPE_FLOAT },
    { "lowPowerMode", TYPE_INT32 },
    { "depthStreamEnabled", TYPE_INT32 },
    { "irStreamMode", TYPE_INT32 },
    { "activePhysicalId", TYPE_BYTE },
};

vendor_tag_info_t qcamera3_diagnostics[QCAMERA3_DIAGNOSTICS_END -
        QCAMERA3_DIAGNOSTICS_START] = {
    { "videoSceneTimeline", TYPE_INT64 },
    { "stageRecords", TYPE_INT32 },
    { "hardwareLevelAdvertised", TYPE_INT32 },
    { "occlusionOccluded", TYPE_INT32 },
    { "occlusionConfidence", TYPE_INT32 },
    { "occlusionReason", TYPE_INT32 },
    { "occlusionCore", TYPE_INT32 },
    { "vstabPath", TYPE_INT32 },
    { "vstabReason", TYPE_INT32 },
    { "vstabGyroCount", TYPE_INT32 },
    { "vstabCoreStatus", TYPE_INT32 },
};

vendor_tag_info_t qcamera3_ar[QCAMERA3_AR_END - QCAMERA3_AR_START] = {
    { "arTrackingEnable", TYPE_INT32 },
    { "arTrackingMode", TYPE_INT32 },
    { "arPose", TYPE_FLOAT },
    { "arPoseConfidence", TYPE_FLOAT },
    { "arTrackingSupported", TYPE_BYTE },
    { "arTrackingStatus", TYPE_INT32 },
    { "arGravityAided", TYPE_BYTE },
};

vendor_tag_info_t qcamera3_sat_cam[QCAMERA3_SAT_MODE_END -
        QCAMERA3_SAT_MODE_START] = {
    { "on", TYPE_BYTE }
};

vendor_tag_info_t qcamera3_eis_flush[QCAMERA3_EIS_FLUSH_END -
        QCAMERA3_EIS_FLUSH_START] = {
    { "endOfStream", TYPE_BYTE }
};

vendor_tag_info_t *qcamera3_tag_info[QCAMERA3_SECTIONS_END -
        VENDOR_SECTION] = {
    qcamera3_privatedata,
    qcamera3_cds,
    qcamera3_crop,
    qcamera3_temporal_denoise,
    qcamera3_iso_exp_priority,
    qcamera3_saturation,
    qcamera3_exposure_metering,
    qcamera3_av_timer,
    qcamera3_dualcam_link_meta_data,
    qcamera3_hal_privatedata,
    qcamera3_jpep_encode_crop,
    qcamera3_video_hdr,
    qcamera3_ir,
    qcamera3_aec_speed,
    qcamera3_awb_speed,
    qcamera3_instant_aec,
    qcamera3_sharpness,
    qcamera3_histogram,
    qcamera3_binning_correction,
    qcamera3_stats,
    qcamera3_simultaneous_camera,
    qcamera3_quadra_cfa,
    qcamera3_bokeh,
    qcamera3_logical_cam,
    qcamera3_manual_wb,
    qcamera3_sat_cam,
    qcamera3_eis_flush,
    qcamera3_swmfnr,
    qcamera3_native_algorithms,
    qcamera3_postprocess,
    qcamera3_stream,
    qcamera3_diagnostics,
    qcamera3_ar,
};

uint32_t qcamera3_all_tags[] = {
    // QCAMERA3_PRIVATEDATA
    (uint32_t)QCAMERA3_PRIVATEDATA_REPROCESS,

    // QCAMERA3_CDS
    (uint32_t)QCAMERA3_CDS_MODE,
    (uint32_t)QCAMERA3_CDS_INFO,

    // QCAMERA3_CROP
    (uint32_t)QCAMERA3_CROP_COUNT_REPROCESS,
    (uint32_t)QCAMERA3_CROP_REPROCESS,
    (uint32_t)QCAMERA3_CROP_ROI_MAP_REPROCESS,

    // QCAMERA3_TEMPORAL_DENOISE
    (uint32_t)QCAMERA3_TEMPORAL_DENOISE_ENABLE,
    (uint32_t)QCAMERA3_TEMPORAL_DENOISE_PROCESS_TYPE,

    // QCAMERA3_ISO_EXP_PRIORITY
    (uint32_t)QCAMERA3_USE_ISO_EXP_PRIORITY,
    (uint32_t)QCAMERA3_SELECT_PRIORITY,
    (uint32_t)QCAMERA3_ISO_AVAILABLE_MODES,
    (uint32_t)QCAMERA3_EXP_TIME_RANGE,

    // QCAMERA3_SATURATION
    (uint32_t)QCAMERA3_USE_SATURATION,
    (uint32_t)QCAMERA3_SATURATION_RANGE,

    // QCAMERA3_EXPOSURE_METERING
    (uint32_t)QCAMERA3_EXPOSURE_METER,
    (uint32_t)QCAMERA3_EXPOSURE_METER_AVAILABLE_MODES,

    //QCAMERA3_AVTIMER
    (uint32_t)QCAMERA3_USE_AV_TIMER,

    // QCAMERA3_DUALCAM_LINK_META_DATA
    (uint32_t)QCAMERA3_DUALCAM_LINK_ENABLE,
    (uint32_t)QCAMERA3_DUALCAM_LINK_IS_MAIN,
    (uint32_t)QCAMERA3_DUALCAM_LINK_RELATED_CAMERA_ID,

    // QCAMERA3_HAL_PRIVATEDATA
    (uint32_t)QCAMERA3_HAL_PRIVATEDATA_REPROCESS_FLAGS,
    (uint32_t)QCAMERA3_HAL_PRIVATEDATA_REPROCESS_DATA_BLOB,
    (uint32_t)QCAMERA3_HAL_PRIVATEDATA_EXIF_DEBUG_DATA_BLOB,

    // QCAMERA3_JPEG_ENCODE_CROP
    (uint32_t)QCAMERA3_JPEG_ENCODE_CROP_ENABLE,
    (uint32_t)QCAMERA3_JPEG_ENCODE_CROP_RECT,
    (uint32_t)QCAMERA3_JPEG_ENCODE_CROP_ROI,

    // QCAMERA3_VIDEO_HDR
    (uint32_t)QCAMERA3_VIDEO_HDR_MODE,
    (uint32_t)QCAMERA3_AVAILABLE_VIDEO_HDR_MODES,

    // QCAMERA3_IR_MODE_ENABLE
    (uint32_t)QCAMERA3_IR_MODE,
    (uint32_t)QCAMERA3_IR_AVAILABLE_MODES,

    //QCAMERA3_AEC_CONVERGENCE_SPEED
    (uint32_t)QCAMERA3_AEC_CONVERGENCE_SPEED,

    //QCAMERA3_AWB_CONVERGENCE_SPEED
    (uint32_t)QCAMERA3_AWB_CONVERGENCE_SPEED,

    // QCAMERA3_INSTANT_AEC
    (uint32_t)QCAMERA3_INSTANT_AEC_MODE,
    (uint32_t)QCAMERA3_INSTANT_AEC_AVAILABLE_MODES,

    //QCAMERA3_SHARPNESS
    (uint32_t)QCAMERA3_SHARPNESS_STRENGTH,
    (uint32_t)QCAMERA3_SHARPNESS_RANGE,

    //QCAMERA3_HISTOGRAM
    (uint32_t)QCAMERA3_HISTOGRAM_MODE,
    (uint32_t)QCAMERA3_HISTOGRAM_BUCKETS,
    (uint32_t)QCAMERA3_HISTOGRAM_MAX_COUNT,
    (uint32_t)QCAMERA3_HISTOGRAM_STATS,

    // QCAMERA3_BINNING_CORRECTION_END
    (uint32_t)QCAMERA3_BINNING_CORRECTION_MODE,
    (uint32_t)QCAMERA3_AVAILABLE_BINNING_CORRECTION_MODES,

    // QCAMERA3_STATS
    (uint32_t)QCAMERA3_STATS_IS_HDR_SCENE,
    (uint32_t)QCAMERA3_STATS_IS_HDR_SCENE_VALUES,
    (uint32_t)QCAMERA3_STATS_IS_HDR_SCENE_CONFIDENCE,
    (uint32_t)QCAMERA3_STATS_IS_HDR_SCENE_CONFIDENCE_RANGE,
    (uint32_t)QCAMERA3_STATS_BSGC_AVAILABLE,
    (uint32_t)QCAMERA3_STATS_BLINK_DETECTED,
    (uint32_t)QCAMERA3_STATS_BLINK_DEGREE,
    (uint32_t)QCAMERA3_STATS_SMILE_DEGREE,
    (uint32_t)QCAMERA3_STATS_SMILE_CONFIDENCE,
    (uint32_t)QCAMERA3_STATS_GAZE_ANGLE,
    (uint32_t)QCAMERA3_STATS_GAZE_DIRECTION,
    (uint32_t)QCAMERA3_STATS_GAZE_DEGREE,

    // QCAMERA3_SIMULTANEOUS_CAMERA
    (uint32_t)QCAMERA3_SIMULTANEOUS_CAMERA_VFE1_RESERVED_RDI,

    // QCAMERA3_QUADRA_CFA_DATA
    (uint32_t)QCAMERA3_IS_QUADRA_CFA_SENSOR,
    (uint32_t)QCAMERA3_SUPPORT_QUADRA_CFA_DIM,

    //QCAMERA3_BOKEH
    (uint32_t)QCAMERA3_BOKEH_ENABLE,
    (uint32_t)QCAMERA3_BOKEH_BLURLEVEL,
    (uint32_t)QCAMERA3_BOKEH_STATUS,

    //QCAMERA3_LOGICAL_CAM
    (uint32_t)QCAMERA3_LOGICAL_CAM_MODE,

    //QCAMERA3_MANUAL_WB
    (uint32_t)QCAMERA3_MANUAL_WB_MODE,
    (uint32_t)QCAMERA3_MANUAL_WB_CCT_RANGE,
    (uint32_t)QCAMERA3_MANUAL_WB_GAINS_RANGE,
    (uint32_t)QCAMERA3_MANUAL_WB_CCT,
    (uint32_t)QCAMERA3_MANUAL_WB_GAINS,
    //QCAMERA3_SAT
    (uint32_t)QCAMERA3_SAT_MODE_ON,
    (uint32_t)QCAMERA3_EIS_FLUSH_ON,
    //QCAMERA3 SWMFNR
    (uint32_t)QCAMERA3_SWMFNR_ENABLE,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHM_MODE,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHM_BEAUTY_STRENGTH,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHM_AVAILABLE_MODES,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHM_FILTER_TYPE,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHM_FILTER_FLIP_MODE,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHM_AVAILABLE_FILTER_TYPES,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHMS_CONTINUOUS_ENABLED,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHMS_CONTINUOUS_BEAUTY_STRENGTH,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHMS_CONTINUOUS_FILTER_TYPE,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHMS_CONTINUOUS_TONE_STRENGTH,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHMS_CONTINUOUS_AVAILABLE_EFFECTS,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHMS_CONTINUOUS_MAX_PIXELS,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHM_SCENE_STRENGTH,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHMS_CONTINUOUS_SCENE_STRENGTH,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHMS_CONTINUOUS_SCENE_RESULT,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHM_RELIGHT_STRENGTH,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHM_RELIGHT_AZIMUTH,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHM_RELIGHT_ELEVATION,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHM_FRONT_BEAUTY_STRENGTH,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHM_FRONT_BEAUTY_FEATURES,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHM_PICBEST_FRAMES,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHM_PICBEST_STRENGTH,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHM_PICBEST_OPEN_EYE_ONLY,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHM_BLURLESS_THRESHOLD,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHM_FACE_INFO_MODE,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHM_IMAGE_ADJUST,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHM_SINGLE_BOKEH_STRENGTH,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHM_SINGLE_BOKEH_SPOT_MODE,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHM_SINGLE_BOKEH_SPOT,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHM_PORTRAIT_LIGHT_MODE,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHM_PORTRAIT_LIGHT_STRENGTH,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHM_DOC_MODE,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHM_DOC_STRENGTH,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHM_AUTO_HDR_ENABLE,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHM_AUTO_NIGHT_ENABLE,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHM_DOUBLE_EXPOSURE_MODE,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHM_DOUBLE_EXPOSURE_STRENGTH,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHM_DOUBLE_EXPOSURE_TIME_NS,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHM_LONG_EXPOSURE_ENABLE,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHM_REMOSAIC_ENABLE,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHM_EXIF_COMMENT,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHM_WATERMARK_ASSET,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHM_STICKER_ASSET,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHM_STICKER_RECT,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHM_MIRROR_PREVIEW,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHM_APP_POLICY_MODE,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHM_APP_MASK,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHM_FACE_PREVIEW_PARAM,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHM_DEPTH_QUALITY,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHM_AVAILABLE_DOC_MODES,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHM_MAX_LONG_EXPOSURE_NS,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHM_LONG_EXPOSURE_RANGE_NS,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHM_AVAILABLE_REMOSAIC_SIZES,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHM_REMOSAIC_SUPPORTED,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHM_DEPTH_SUPPORTED,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHM_AR_TRACKING_SUPPORTED,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHM_PROFESSIONAL_VER,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHM_HLG_VIDEO_SUPPORTED,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHM_NATIVE_TRACE,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHM_FACE_ATTRIBUTES,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHM_ESTIMATED_SNAPSHOT_EXP_TIME,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHM_HOT_PIXEL_MAP_AVAILABLE,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHM_APPLIED_NATIVE_MODE,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHM_PREVIEW_BOKEH_STRENGTH,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHM_PREVIEW_BOKEH_TEMPORAL_SMOOTHING,
    (uint32_t)QCAMERA3_NATIVE_ALGORITHM_AUTO_CAPTURE_REASON,
    (uint32_t)QCAMERA3_POSTPROCESS_SETTINGS,
    (uint32_t)QCAMERA3_POSTPROCESS_WARP_REGIONS,
    (uint32_t)QCAMERA3_POSTPROCESS_OVERLAY,
    (uint32_t)QCAMERA3_POSTPROCESS_WATERMARK,
    (uint32_t)QCAMERA3_POSTPROCESS_FACE_TRACKING,
    (uint32_t)QCAMERA3_POSTPROCESS_WATERMARK_LAYOUT,
    (uint32_t)QCAMERA3_POSTPROCESS_MIRROR,
    (uint32_t)QCAMERA3_POSTPROCESS_EXIF_USER_COMMENT,
    (uint32_t)QCAMERA3_STREAM_SLOW_MOTION_RATIO,
    (uint32_t)QCAMERA3_STREAM_AVAILABLE_SLOW_MOTION_RATIOS,
    (uint32_t)QCAMERA3_STREAM_VIDEO_HDR_PROFILE,
    (uint32_t)QCAMERA3_STREAM_VSTAB_CROP_FACTOR,
    (uint32_t)QCAMERA3_STREAM_VSTAB_OFFSET,
    (uint32_t)QCAMERA3_STREAM_LOGICAL_ZOOM_HINT,
    (uint32_t)QCAMERA3_STREAM_LOW_POWER_MODE,
    (uint32_t)QCAMERA3_STREAM_DEPTH_STREAM_ENABLED,
    (uint32_t)QCAMERA3_STREAM_IR_STREAM_MODE,
    (uint32_t)QCAMERA3_STREAM_ACTIVE_PHYSICAL_ID,
    (uint32_t)QCAMERA3_DIAGNOSTICS_VIDEO_SCENE_TIMELINE,
    (uint32_t)QCAMERA3_DIAGNOSTICS_STAGE_RECORDS,
    (uint32_t)QCAMERA3_DIAGNOSTICS_HARDWARE_LEVEL_ADVERTISED,
    (uint32_t)QCAMERA3_DIAGNOSTICS_OCCLUSION_OCCLUDED,
    (uint32_t)QCAMERA3_DIAGNOSTICS_OCCLUSION_CONFIDENCE,
    (uint32_t)QCAMERA3_DIAGNOSTICS_OCCLUSION_REASON,
    (uint32_t)QCAMERA3_DIAGNOSTICS_OCCLUSION_CORE,
    (uint32_t)QCAMERA3_DIAGNOSTICS_VSTAB_PATH,
    (uint32_t)QCAMERA3_DIAGNOSTICS_VSTAB_REASON,
    (uint32_t)QCAMERA3_DIAGNOSTICS_VSTAB_GYRO_COUNT,
    (uint32_t)QCAMERA3_DIAGNOSTICS_VSTAB_CORE_STATUS,
    (uint32_t)QCAMERA3_AR_TRACKING_ENABLE,
    (uint32_t)QCAMERA3_AR_TRACKING_MODE,
    (uint32_t)QCAMERA3_AR_POSE,
    (uint32_t)QCAMERA3_AR_POSE_CONFIDENCE,
    (uint32_t)QCAMERA3_AR_TRACKING_SUPPORTED,
    (uint32_t)QCAMERA3_AR_TRACKING_STATUS,
    (uint32_t)QCAMERA3_AR_GRAVITY_AIDED
};

const vendor_tag_ops_t* QCamera3VendorTags::Ops = NULL;

/*===========================================================================
 * FUNCTION   : get_vendor_tag_ops
 *
 * DESCRIPTION: Get the metadata vendor tag function pointers
 *
 * PARAMETERS :
 *    @ops   : function pointer table to be filled by HAL
 *
 *
 * RETURN     : NONE
 *==========================================================================*/
void QCamera3VendorTags::get_vendor_tag_ops(
                                vendor_tag_ops_t* ops)
{
    LOGL("E");

    Ops = ops;

    ops->get_tag_count = get_tag_count;
    ops->get_all_tags = get_all_tags;
    ops->get_section_name = get_section_name;
    ops->get_tag_name = get_tag_name;
    ops->get_tag_type = get_tag_type;
    ops->reserved[0] = NULL;

    LOGL("X");
    return;
}

/* libcamera_metadata exports this entry point, but the public
 * system/camera_metadata.h does not declare it (AOSP declares it in the
 * private camera_metadata_hidden.h and the provider registers its
 * VendorTagDescriptor through it). Declared locally so this vendor module
 * does not need the private header. */
extern "C" int set_camera_metadata_vendor_ops(const vendor_tag_ops_t *ops);

void QCamera3VendorTags::registerWithMetadata()
{
    // Process-lifetime storage: libcamera_metadata keeps the pointer.
    static vendor_tag_ops_t sOps;
    if (Ops == &sOps) return; // already registered by this module
    get_vendor_tag_ops(&sOps);
    set_camera_metadata_vendor_ops(&sOps);
}

/*===========================================================================
 * FUNCTION   : get_tag_count
 *
 * DESCRIPTION: Get number of vendor tags supported
 *
 * PARAMETERS :
 *    @ops   :  Vendor tag ops data structure
 *
 *
 * RETURN     : Number of vendor tags supported
 *==========================================================================*/

int QCamera3VendorTags::get_tag_count(
                const vendor_tag_ops_t * ops)
{
    size_t count = 0;
    if (ops == Ops)
        count = sizeof(qcamera3_all_tags)/sizeof(qcamera3_all_tags[0]);

    LOGL("count is %d", count);
    return (int)count;
}

/*===========================================================================
 * FUNCTION   : get_all_tags
 *
 * DESCRIPTION: Fill array with all supported vendor tags
 *
 * PARAMETERS :
 *    @ops      :  Vendor tag ops data structure
 *    @tag_array:  array of metadata tags
 *
 * RETURN     : Success: the section name of the specific tag
 *              Failure: NULL
 *==========================================================================*/
void QCamera3VendorTags::get_all_tags(
                const vendor_tag_ops_t * ops,
                uint32_t *g_array)
{
    if (ops != Ops)
        return;

    for (size_t i = 0;
            i < sizeof(qcamera3_all_tags)/sizeof(qcamera3_all_tags[0]);
            i++) {
        g_array[i] = qcamera3_all_tags[i];
        LOGD("g_array[%d] is %d", i, g_array[i]);
    }
}

/*===========================================================================
 * FUNCTION   : get_section_name
 *
 * DESCRIPTION: Get section name for vendor tag
 *
 * PARAMETERS :
 *    @ops   :  Vendor tag ops structure
 *    @tag   :  Vendor specific tag
 *
 *
 * RETURN     : Success: the section name of the specific tag
 *              Failure: NULL
 *==========================================================================*/

const char* QCamera3VendorTags::get_section_name(
                const vendor_tag_ops_t * ops,
                uint32_t tag)
{
    LOGL("E");
    if (ops != Ops)
        return NULL;

    const char *ret;
    uint32_t section = tag >> 16;

    if (section < VENDOR_SECTION || section >= QCAMERA3_SECTIONS_END)
        ret = NULL;
    else
        ret = qcamera3_ext_section_names[section - VENDOR_SECTION];

    if (ret)
        LOGL("section_name[%d] is %s", tag, ret);
    LOGL("X");
    return ret;
}

/*===========================================================================
 * FUNCTION   : get_tag_name
 *
 * DESCRIPTION: Get name of a vendor specific tag
 *
 * PARAMETERS :
 *    @tag   :  Vendor specific tag
 *
 *
 * RETURN     : Success: the name of the specific tag
 *              Failure: NULL
 *==========================================================================*/
const char* QCamera3VendorTags::get_tag_name(
                const vendor_tag_ops_t * ops,
                uint32_t tag)
{
    LOGL("E");
    const char *ret;
    uint32_t section = tag >> 16;
    uint32_t section_index = section - VENDOR_SECTION;
    uint32_t tag_index = tag & 0xFFFF;

    if (ops != Ops) {
        ret = NULL;
        goto done;
    }

    if (section < VENDOR_SECTION || section >= QCAMERA3_SECTIONS_END)
        ret = NULL;
    else if (tag >= (uint32_t)qcamera3_ext3_section_bounds[section_index])
        ret = NULL;
    else
        ret = qcamera3_tag_info[section_index][tag_index].tag_name;

    if (ret)
        LOGL("tag name for tag %d is %s", tag, ret);
    LOGL("X");

done:
    return ret;
}

/*===========================================================================
 * FUNCTION   : get_tag_type
 *
 * DESCRIPTION: Get type of a vendor specific tag
 *
 * PARAMETERS :
 *    @tag   :  Vendor specific tag
 *
 *
 * RETURN     : Success: the type of the specific tag
 *              Failure: -1
 *==========================================================================*/
int QCamera3VendorTags::get_tag_type(
                const vendor_tag_ops_t *ops,
                uint32_t tag)
{
    LOGL("E");
    int ret;
    uint32_t section = tag >> 16;
    uint32_t section_index = section - VENDOR_SECTION;
    uint32_t tag_index = tag & 0xFFFF;

    if (ops != Ops) {
        ret = -1;
        goto done;
    }
    if (section < VENDOR_SECTION || section >= QCAMERA3_SECTIONS_END)
        ret = -1;
    else if (tag >= (uint32_t )qcamera3_ext3_section_bounds[section_index])
        ret = -1;
    else
        ret = qcamera3_tag_info[section_index][tag_index].tag_type;

    LOGL("tag type for tag %d is %d", tag, ret);
    LOGL("X");
done:
    return ret;
}

}; //end namespace qcamera
