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

#ifndef __QCAMERA3VENDORTAGS_H__
#define __QCAMERA3VENDORTAGS_H__

// Camera dependencies
#include "system/camera_metadata.h"

namespace qcamera {

enum qcamera3_ext_section {
    QCAMERA3_PRIVATEDATA = VENDOR_SECTION,
    QCAMERA3_CDS,
    QCAMERA3_CROP,
    QCAMERA3_TEMPORAL_DENOISE,
    QCAMERA3_ISO_EXP_PRIORITY,
    QCAMERA3_SATURATION,
    QCAMERA3_EXPOSURE_METERING_MODE,
    QCAMERA3_AV_TIMER,
    QCAMERA3_DUALCAM_LINK_META_DATA,
    QCAMERA3_HAL_PRIVATEDATA,
    QCAMERA3_JPEG_ENCODE_CROP,
    QCAMERA3_VIDEO_HDR,
    QCAMERA3_IR,
    QCAMERA3_AEC_CONVERGENCE,
    QCAMERA3_AWB_CONVERGENCE,
    QCAMERA3_INSTANT_AEC,
    QCAMERA3_SHARPNESS_DATA,
    QCAMERA3_HISTOGRAM,
    QCAMERA3_BINNING_CORRECTION,
    QCAMERA3_STATS,
    QCAMERA3_SIMULTANEOUS_CAMERA,
    QCAMERA3_QUADRA_CFA_DATA,
    QCAMERA3_BOKEH,
    QCAMERA3_LOGICAL_CAM,
    QCAMERA3_MANUAL_WB,
    QCAMERA3_SAT_MODE,
    QCAMERA3_EIS_FLUSH,
    QCAMERA3_SWMFNR,
    QCAMERA3_NATIVE_ALGORITHMS,
    QCAMERA3_POSTPROCESS,
    // Appended after POSTPROCESS so existing section numbers stay fixed.
    QCAMERA3_STREAM,
    QCAMERA3_DIAGNOSTICS,
    QCAMERA3_AR,
    QCAMERA3_SECTIONS_END,
};

enum qcamera3_ext_section_ranges {
    QCAMERA3_PRIVATEDATA_START = QCAMERA3_PRIVATEDATA << 16,
    QCAMERA3_CDS_START = QCAMERA3_CDS << 16,
    QCAMERA3_CROP_START = QCAMERA3_CROP << 16,
    QCAMERA3_TEMPORAL_DENOISE_START = QCAMERA3_TEMPORAL_DENOISE << 16,
    QCAMERA3_ISO_EXP_PRIORITY_START = QCAMERA3_ISO_EXP_PRIORITY << 16,
    QCAMERA3_SATURATION_START = QCAMERA3_SATURATION << 16,
    QCAMERA3_EXPOSURE_METER_START = QCAMERA3_EXPOSURE_METERING_MODE << 16,
    QCAMERA3_AV_TIMER_START = QCAMERA3_AV_TIMER << 16,
    QCAMERA3_DUALCAM_LINK_META_DATA_START = QCAMERA3_DUALCAM_LINK_META_DATA << 16,
    QCAMERA3_HAL_PRIVATEDATA_START = QCAMERA3_HAL_PRIVATEDATA << 16,
    QCAMERA3_JPEG_ENCODE_CROP_START = QCAMERA3_JPEG_ENCODE_CROP << 16,
    QCAMERA3_VIDEO_HDR_START = QCAMERA3_VIDEO_HDR << 16,
    QCAMERA3_IR_START = QCAMERA3_IR << 16,
    QCAMERA3_AEC_CONVERGENCE_SPEED_START = QCAMERA3_AEC_CONVERGENCE << 16,
    QCAMERA3_AWB_CONVERGENCE_SPEED_START = QCAMERA3_AWB_CONVERGENCE << 16,
    QCAMERA3_INSTANT_AEC_START = QCAMERA3_INSTANT_AEC << 16,
    QCAMERA3_SHARPNESS_START = QCAMERA3_SHARPNESS_DATA << 16,
    QCAMERA3_HISTOGRAM_START = QCAMERA3_HISTOGRAM << 16,
    QCAMERA3_BINNING_CORRECTION_START = QCAMERA3_BINNING_CORRECTION << 16,
    QCAMERA3_STATS_START = QCAMERA3_STATS << 16,
    QCAMERA3_SIMULTANEOUS_CAMERA_START = QCAMERA3_SIMULTANEOUS_CAMERA << 16,
    QCAMERA3_QUADRA_CFA_DATA_START = QCAMERA3_QUADRA_CFA_DATA << 16,
    QCAMERA3_BOKEH_START = QCAMERA3_BOKEH << 16,
    QCAMERA3_LOGICAL_CAM_START = QCAMERA3_LOGICAL_CAM << 16,
    QCAMERA3_MANUAL_WB_START = QCAMERA3_MANUAL_WB << 16,
    QCAMERA3_SAT_MODE_START = QCAMERA3_SAT_MODE << 16,
    QCAMERA3_EIS_FLUSH_START = QCAMERA3_EIS_FLUSH << 16,
    QCAMERA3_SWMFNR_START = QCAMERA3_SWMFNR << 16,
    QCAMERA3_NATIVE_ALGORITHMS_START = QCAMERA3_NATIVE_ALGORITHMS << 16,
    QCAMERA3_POSTPROCESS_START = QCAMERA3_POSTPROCESS << 16,
    QCAMERA3_STREAM_START = QCAMERA3_STREAM << 16,
    QCAMERA3_DIAGNOSTICS_START = QCAMERA3_DIAGNOSTICS << 16,
    QCAMERA3_AR_START = QCAMERA3_AR << 16,
};

enum qcamera3_ext_tags {
    QCAMERA3_PRIVATEDATA_REPROCESS = QCAMERA3_PRIVATEDATA_START,
    QCAMERA3_PRIVATEDATA_END,
    QCAMERA3_CDS_MODE = QCAMERA3_CDS_START,
    QCAMERA3_CDS_INFO,
    QCAMERA3_CDS_END,

    QCAMERA3_CROP_COUNT_REPROCESS = QCAMERA3_CROP_START,
    QCAMERA3_CROP_REPROCESS,
    QCAMERA3_CROP_ROI_MAP_REPROCESS,
    QCAMERA3_CROP_END,

    QCAMERA3_TEMPORAL_DENOISE_ENABLE = QCAMERA3_TEMPORAL_DENOISE_START,
    QCAMERA3_TEMPORAL_DENOISE_PROCESS_TYPE,
    QCAMERA3_TEMPORAL_DENOISE_END,

    QCAMERA3_USE_ISO_EXP_PRIORITY = QCAMERA3_ISO_EXP_PRIORITY_START,
    QCAMERA3_SELECT_PRIORITY,
    /* Property Name:  org.codeaurora.qcamera3.iso_exp_priority.iso_available_modes
       Type: int32[8]
       Description: Available iso mode values
       iso_available_modes[0] = 0 (auto)
       iso_available_modes[1] = 1 (deblur)
       iso_available_modes[2] = 2 (100)
       iso_available_modes[3] = 3 (200)
       iso_available_modes[4] = 4 (400)
       iso_available_modes[5] = 5 (800)
       iso_available_modes[6] = 6 (1600)
       iso_available_modes[7] = 7 (3200)
    */
    QCAMERA3_ISO_AVAILABLE_MODES,
    /* Property Name:  org.codeaurora.qcamera3.iso_exp_priority.exposure_time_range
       Type: int64[2]
       Description: Min/max exposure time values (ns)
       exposure_time_range[0] = min
       exposure_time_range[1] = max
    */
    QCAMERA3_EXP_TIME_RANGE,
    QCAMERA3_ISO_EXP_PRIORITY_END,

    QCAMERA3_USE_SATURATION = QCAMERA3_SATURATION_START,
    /* Property Name:  org.codeaurora.qcamera3.saturation.range
       Type: int32[4]
       Description: Range of possible saturation values
       range[0] = min
       range[1] = max
       range[2] = default
       range[3] = step
    */
    QCAMERA3_SATURATION_RANGE,
    QCAMERA3_SATURATION_END,

    QCAMERA3_EXPOSURE_METER = QCAMERA3_EXPOSURE_METER_START,
    /* Property Name:  org.codeaurora.qcamera3.exposure_metering.available_modes
       Type: int32[7]
       Description: Available exposure metering mode values
       available_modes[0] = 0 (FRAME_AVERAGE)
       available_modes[1] = 1 (CENTER_WEIGHTED)
       available_modes[2] = 2 (SPOT_METERING)
       available_modes[3] = 3 (SMART_METERING)
       available_modes[4] = 4 (USER_METERING)
       available_modes[5] = 5 (SPOT_METERING_ADV)
       available_modes[6] = 6 (CENTER_WEIGHTED_ADV)
    */
    QCAMERA3_EXPOSURE_METER_AVAILABLE_MODES,
    QCAMERA3_EXPOSURE_METER_END,

    QCAMERA3_USE_AV_TIMER = QCAMERA3_AV_TIMER_START,
    QCAMERA3_AV_TIMER_END,

    QCAMERA3_DUALCAM_LINK_ENABLE = QCAMERA3_DUALCAM_LINK_META_DATA_START,
    QCAMERA3_DUALCAM_LINK_IS_MAIN,
    QCAMERA3_DUALCAM_LINK_RELATED_CAMERA_ID,
    QCAMERA3_DUALCAM_LINK_META_DATA_END,

    QCAMERA3_HAL_PRIVATEDATA_REPROCESS_FLAGS = QCAMERA3_HAL_PRIVATEDATA_START,
    QCAMERA3_HAL_PRIVATEDATA_REPROCESS_DATA_BLOB,
    QCAMERA3_HAL_PRIVATEDATA_EXIF_DEBUG_DATA_BLOB,

    QCAMERA3_HAL_PRIVATEDATA_END,

    /* Property Name:  org.codeaurora.qcamera3.jpeg_encode_crop.enable
       Type: byte
       Description: If JPEG crop is enable
    */
    QCAMERA3_JPEG_ENCODE_CROP_ENABLE = QCAMERA3_JPEG_ENCODE_CROP_START,
    /* Property Name:  org.codeaurora.qcamera3.jpeg_encode_crop.rect
       Type: int32[4]
       Description: Crop image into size width x height
                    from [left, top] coordinate
       rect[0] = left
       rect[1] = top
       rect[2] = width
       rect[3] = height
    */
    QCAMERA3_JPEG_ENCODE_CROP_RECT,
    /* Property Name:  org.codeaurora.qcamera3.jpeg_encode_crop.roi
       Type: int32[4]
       Description: Scale the crop image into size width x height
                    from [left, top] coordinate.
       roi[0] = left
       roi[1] = top
       roi[2] = width
       roi[3] = height
    */
    QCAMERA3_JPEG_ENCODE_CROP_ROI,
    QCAMERA3_JPEG_ENCODE_CROP_END,

    QCAMERA3_VIDEO_HDR_MODE = QCAMERA3_VIDEO_HDR_START,
    QCAMERA3_AVAILABLE_VIDEO_HDR_MODES,
    QCAMERA3_VIDEO_HDR_END,

    QCAMERA3_IR_MODE = QCAMERA3_IR_START,
    QCAMERA3_IR_AVAILABLE_MODES,
    QCAMERA3_IR_END,

    QCAMERA3_AEC_CONVERGENCE_SPEED = QCAMERA3_AEC_CONVERGENCE_SPEED_START,
    QCAMERA3_AEC_CONVERGENCE_SPEED_END,

    QCAMERA3_AWB_CONVERGENCE_SPEED = QCAMERA3_AWB_CONVERGENCE_SPEED_START,
    QCAMERA3_AWB_CONVERGENCE_SPEED_END,

    QCAMERA3_INSTANT_AEC_MODE = QCAMERA3_INSTANT_AEC_START,
    QCAMERA3_INSTANT_AEC_AVAILABLE_MODES,
    QCAMERA3_INSTANT_AEC_END,

    QCAMERA3_SHARPNESS_STRENGTH = QCAMERA3_SHARPNESS_START,
    QCAMERA3_SHARPNESS_RANGE,
    QCAMERA3_SHARPNESS_END,

    QCAMERA3_HISTOGRAM_MODE = QCAMERA3_HISTOGRAM_START,
    QCAMERA3_HISTOGRAM_BUCKETS,
    QCAMERA3_HISTOGRAM_MAX_COUNT,
    QCAMERA3_HISTOGRAM_STATS,
    QCAMERA3_HISTOGRAM_END,

    QCAMERA3_BINNING_CORRECTION_MODE = QCAMERA3_BINNING_CORRECTION_START,
    QCAMERA3_AVAILABLE_BINNING_CORRECTION_MODES,
    QCAMERA3_BINNING_CORRECTION_END,

    QCAMERA3_STATS_IS_HDR_SCENE = QCAMERA3_STATS_START,
    /* Property Name:  org.codeaurora.qcamera3.stats.is_hdr_scene_values
       Type: byte[2]
       Description: Possible values for is_hdr_scene
       roi[0] = 0 (false)
       roi[1] = 1 (true)
    */
    QCAMERA3_STATS_IS_HDR_SCENE_VALUES,
    QCAMERA3_STATS_IS_HDR_SCENE_CONFIDENCE,
    /* Property Name:  org.codeaurora.qcamera3.stats.is_hdr_scene_confidence_range
       Type: float[2]
       Description: Range of values for is_hdr_scene_confidence
       roi[0] = 0.0 (min)
       roi[1] = 1.0 (max)
    */
    QCAMERA3_STATS_IS_HDR_SCENE_CONFIDENCE_RANGE,
    QCAMERA3_STATS_BSGC_AVAILABLE,
    QCAMERA3_STATS_BLINK_DETECTED,
    /* Property Name:  org.codeaurora.qcamera3.stats.blink_degree
       Type: int8[2]
       Description: Blink degree
       blink_degree[0] = left
       blink_degree[1] = right
    */
    QCAMERA3_STATS_BLINK_DEGREE,
    QCAMERA3_STATS_SMILE_DEGREE,
    QCAMERA3_STATS_SMILE_CONFIDENCE,
    QCAMERA3_STATS_GAZE_ANGLE,
    /* Property Name:  org.codeaurora.qcamera3.stats.gaze_direction
       Type: int32[3]
       Description: Gaze direction
       gaze_direction[0] = up-down
       gaze_direction[1] = left-right
       gaze_direction[2] = roll
    */
    QCAMERA3_STATS_GAZE_DIRECTION,
    /* Property Name:  org.codeaurora.qcamera3.stats.gaze_degree
       Type: int8[2]
       Description: Gaze degree
       gaze_degree[0] = left-right
       gaze_degree[1] = top-bottom
    */
    QCAMERA3_STATS_GAZE_DEGREE,
    QCAMERA3_STATS_END,

    QCAMERA3_SIMULTANEOUS_CAMERA_VFE1_RESERVED_RDI = QCAMERA3_SIMULTANEOUS_CAMERA_START,
    /* Property Name:  org.codeaurora.qcamera3.simultaneous_camera.vfe1_reserved_rdi
       Type: int32
       Description: number of RDI interface reserved on VFE1(the smaller VFE)
       Details: It will guarantee the available RDI interfaces for RDI streams,
                and any RDI stream requests that exceed the reserved number will
                be allocated  to the other VFE.
                Range from -1 to 3, -1 means the feature is disabled.
    */
    QCAMERA3_SIMULTANEOUS_CAMERA_END,

    QCAMERA3_IS_QUADRA_CFA_SENSOR = QCAMERA3_QUADRA_CFA_DATA_START,
    QCAMERA3_SUPPORT_QUADRA_CFA_DIM,
    QCAMERA3_QUADRA_CFA_DATA_END,

    QCAMERA3_BOKEH_ENABLE = QCAMERA3_BOKEH_START,
    QCAMERA3_BOKEH_BLURLEVEL,
    QCAMERA3_BOKEH_STATUS,
    QCAMERA3_BOKEH_END,

    QCAMERA3_LOGICAL_CAM_MODE = QCAMERA3_LOGICAL_CAM_START,
    QCAMERA3_LOGICAL_CAM_END,

    QCAMERA3_MANUAL_WB_MODE = QCAMERA3_MANUAL_WB_START,
    QCAMERA3_MANUAL_WB_CCT_RANGE,
    QCAMERA3_MANUAL_WB_GAINS_RANGE,
    QCAMERA3_MANUAL_WB_CCT,
    QCAMERA3_MANUAL_WB_GAINS,
    QCAMERA3_MANUAL_WB_END,

    QCAMERA3_SAT_MODE_ON = QCAMERA3_SAT_MODE_START,
    QCAMERA3_SAT_MODE_END,

    QCAMERA3_EIS_FLUSH_ON = QCAMERA3_EIS_FLUSH_START,
    QCAMERA3_EIS_FLUSH_END,

    QCAMERA3_SWMFNR_ENABLE = QCAMERA3_SWMFNR_START,
    QCAMERA3_SWMFNR_END,
    QCAMERA3_NATIVE_ALGORITHM_MODE = QCAMERA3_NATIVE_ALGORITHMS_START,
    QCAMERA3_NATIVE_ALGORITHM_BEAUTY_STRENGTH,
    QCAMERA3_NATIVE_ALGORITHM_AVAILABLE_MODES,
    QCAMERA3_NATIVE_ALGORITHM_FILTER_TYPE,
    QCAMERA3_NATIVE_ALGORITHM_FILTER_FLIP_MODE,
    QCAMERA3_NATIVE_ALGORITHM_AVAILABLE_FILTER_TYPES,
    QCAMERA3_NATIVE_ALGORITHM_SCENE_STRENGTH,
    QCAMERA3_NATIVE_ALGORITHM_RELIGHT_STRENGTH,
    QCAMERA3_NATIVE_ALGORITHM_RELIGHT_AZIMUTH,
    QCAMERA3_NATIVE_ALGORITHM_RELIGHT_ELEVATION,
    // HAL3 native algorithm stage tags.
    QCAMERA3_NATIVE_ALGORITHM_FRONT_BEAUTY_STRENGTH,      // int32 0..100
    QCAMERA3_NATIVE_ALGORITHM_FRONT_BEAUTY_FEATURES,      // int32[7] per-feature levels
    QCAMERA3_NATIVE_ALGORITHM_PICBEST_FRAMES,              // int32 2..8
    QCAMERA3_NATIVE_ALGORITHM_PICBEST_STRENGTH,            // int32 0..100
    QCAMERA3_NATIVE_ALGORITHM_PICBEST_OPEN_EYE_ONLY,       // int32 0/1
    QCAMERA3_NATIVE_ALGORITHM_BLURLESS_THRESHOLD,          // int32 0..1000
    QCAMERA3_NATIVE_ALGORITHM_FACE_INFO_MODE,              // int32 0/1/2
    QCAMERA3_NATIVE_ALGORITHM_IMAGE_ADJUST,                // int32[6] -100..100
    QCAMERA3_NATIVE_ALGORITHM_SINGLE_BOKEH_STRENGTH,       // int32 0..100
    QCAMERA3_NATIVE_ALGORITHM_SINGLE_BOKEH_SPOT_MODE,      // int32 0/1/2
    QCAMERA3_NATIVE_ALGORITHM_SINGLE_BOKEH_SPOT,           // float[2] 0..1
    QCAMERA3_NATIVE_ALGORITHM_PORTRAIT_LIGHT_MODE,         // int32 0..9
    QCAMERA3_NATIVE_ALGORITHM_PORTRAIT_LIGHT_STRENGTH,     // int32 0..100
    QCAMERA3_NATIVE_ALGORITHM_DOC_MODE,                    // int32 0..3
    QCAMERA3_NATIVE_ALGORITHM_DOC_STRENGTH,                // int32 0..100
    QCAMERA3_NATIVE_ALGORITHM_AUTO_HDR_ENABLE,             // int32 0/1
    QCAMERA3_NATIVE_ALGORITHM_AUTO_NIGHT_ENABLE,           // int32 0/1
    QCAMERA3_NATIVE_ALGORITHM_DOUBLE_EXPOSURE_MODE,        // int32 0/1/2
    QCAMERA3_NATIVE_ALGORITHM_DOUBLE_EXPOSURE_STRENGTH,    // int32 0..100
    QCAMERA3_NATIVE_ALGORITHM_DOUBLE_EXPOSURE_TIME_NS,     // int64
    QCAMERA3_NATIVE_ALGORITHM_LONG_EXPOSURE_ENABLE,        // int32 0/1
    QCAMERA3_NATIVE_ALGORITHM_REMOSAIC_ENABLE,             // int32 0/1
    QCAMERA3_NATIVE_ALGORITHM_EXIF_COMMENT,                // byte UTF-8 <=256
    QCAMERA3_NATIVE_ALGORITHM_WATERMARK_ASSET,             // byte UTF-8 <=256
    QCAMERA3_NATIVE_ALGORITHM_STICKER_ASSET,               // byte UTF-8 <=256
    QCAMERA3_NATIVE_ALGORITHM_STICKER_RECT,                // float[4] 0..1
    QCAMERA3_NATIVE_ALGORITHM_MIRROR_PREVIEW,              // int32 0/1
    QCAMERA3_NATIVE_ALGORITHM_APP_POLICY_MODE,             // int32 0/1
    QCAMERA3_NATIVE_ALGORITHM_APP_MASK,                    // int32 bitmask
    QCAMERA3_NATIVE_ALGORITHM_FACE_PREVIEW_PARAM,          // int32[4]
    QCAMERA3_NATIVE_ALGORITHM_DEPTH_QUALITY,               // int32 0..3
    QCAMERA3_NATIVE_ALGORITHM_AVAILABLE_DOC_MODES,         // int32[] static
    QCAMERA3_NATIVE_ALGORITHM_MAX_LONG_EXPOSURE_NS,        // int64 static
    QCAMERA3_NATIVE_ALGORITHM_LONG_EXPOSURE_RANGE_NS,      // int64[2] static
    QCAMERA3_NATIVE_ALGORITHM_AVAILABLE_REMOSAIC_SIZES,    // int32[] static
    QCAMERA3_NATIVE_ALGORITHM_REMOSAIC_SUPPORTED,          // byte static
    QCAMERA3_NATIVE_ALGORITHM_DEPTH_SUPPORTED,             // byte static
    QCAMERA3_NATIVE_ALGORITHM_AR_TRACKING_SUPPORTED,       // byte static
    QCAMERA3_NATIVE_ALGORITHM_PROFESSIONAL_VER,            // int32 static
    QCAMERA3_NATIVE_ALGORITHM_HLG_VIDEO_SUPPORTED,         // byte static
    QCAMERA3_NATIVE_ALGORITHM_NATIVE_TRACE,                // byte UTF-8 result
    QCAMERA3_NATIVE_ALGORITHM_FACE_ATTRIBUTES,             // int32[4*N] result
    QCAMERA3_NATIVE_ALGORITHM_ESTIMATED_SNAPSHOT_EXP_TIME, // int64 result
    QCAMERA3_NATIVE_ALGORITHM_HOT_PIXEL_MAP_AVAILABLE,     // byte result
    QCAMERA3_NATIVE_ALGORITHM_APPLIED_NATIVE_MODE,         // int32 result
    QCAMERA3_NATIVE_ALGORITHM_PREVIEW_BOKEH_STRENGTH,      // int32 0..100, effect 8
    QCAMERA3_NATIVE_ALGORITHM_PREVIEW_BOKEH_TEMPORAL_SMOOTHING, // int32 0..95
    QCAMERA3_NATIVE_ALGORITHM_AUTO_CAPTURE_REASON,         // int32 result
    QCAMERA3_NATIVE_ALGORITHMS_END,
    // Kept for registration/ABI compatibility; no in-tree consumer.
    QCAMERA3_POSTPROCESS_SETTINGS = QCAMERA3_POSTPROCESS_START,
    QCAMERA3_POSTPROCESS_WARP_REGIONS,
    QCAMERA3_POSTPROCESS_OVERLAY,
    QCAMERA3_POSTPROCESS_WATERMARK,
    QCAMERA3_POSTPROCESS_FACE_TRACKING,
    QCAMERA3_POSTPROCESS_WATERMARK_LAYOUT,   // int32[4] normalized layout
    QCAMERA3_POSTPROCESS_MIRROR,             // int32 0/1
    QCAMERA3_POSTPROCESS_EXIF_USER_COMMENT,  // byte UTF-8 <=256
    QCAMERA3_POSTPROCESS_END,

    // org.lineageos.camera.stream - stream/pipeline controls.
    QCAMERA3_STREAM_SLOW_MOTION_RATIO = QCAMERA3_STREAM_START, // int32 0=off
    QCAMERA3_STREAM_AVAILABLE_SLOW_MOTION_RATIOS,              // int32[] static
    QCAMERA3_STREAM_VIDEO_HDR_PROFILE,                         // int32 0 SDR/1 HLG10
    QCAMERA3_STREAM_VSTAB_CROP_FACTOR,                         // float result
    QCAMERA3_STREAM_VSTAB_OFFSET,                              // float[2] result
    QCAMERA3_STREAM_LOGICAL_ZOOM_HINT,                         // float
    QCAMERA3_STREAM_LOW_POWER_MODE,                            // int32 0/1
    QCAMERA3_STREAM_DEPTH_STREAM_ENABLED,                      // int32 0/1
    QCAMERA3_STREAM_IR_STREAM_MODE,                            // int32 0/1/2
    QCAMERA3_STREAM_ACTIVE_PHYSICAL_ID,                        // byte result UTF-8
    QCAMERA3_STREAM_END,

    // org.lineageos.camera.diagnostics - diagnostics results.
    QCAMERA3_DIAGNOSTICS_VIDEO_SCENE_TIMELINE = QCAMERA3_DIAGNOSTICS_START, // int64[2N]
    QCAMERA3_DIAGNOSTICS_STAGE_RECORDS,                                     // int32[4N]
    QCAMERA3_DIAGNOSTICS_HARDWARE_LEVEL_ADVERTISED,                         // int32
    QCAMERA3_DIAGNOSTICS_OCCLUSION_OCCLUDED,                                // int32 0/1
    QCAMERA3_DIAGNOSTICS_OCCLUSION_CONFIDENCE,                              // int32 0..100
    QCAMERA3_DIAGNOSTICS_OCCLUSION_REASON,                                  // int32
    QCAMERA3_DIAGNOSTICS_OCCLUSION_CORE,                                    // int32
    QCAMERA3_DIAGNOSTICS_VSTAB_PATH,                                        // int32
    QCAMERA3_DIAGNOSTICS_VSTAB_REASON,                                      // int32
    QCAMERA3_DIAGNOSTICS_VSTAB_GYRO_COUNT,                                  // int32
    QCAMERA3_DIAGNOSTICS_VSTAB_CORE_STATUS,                                 // int32
    QCAMERA3_DIAGNOSTICS_VSTAB_RENDERER,                                    // int32 0=CPU 1=GPU
    QCAMERA3_DIAGNOSTICS_VSTAB_GPU_STATUS,                                  // int32
    QCAMERA3_DIAGNOSTICS_END,

    // org.lineageos.camera.ar - orientation/AR tracking.
    QCAMERA3_AR_TRACKING_ENABLE = QCAMERA3_AR_START,   // int32 0/1
    QCAMERA3_AR_TRACKING_MODE,                         // int32 0/1
    QCAMERA3_AR_POSE,                                  // float[7] result
    QCAMERA3_AR_POSE_CONFIDENCE,                       // float result
    QCAMERA3_AR_TRACKING_SUPPORTED,                    // byte static
    QCAMERA3_AR_TRACKING_STATUS,                       // int32 result errno
    QCAMERA3_AR_GRAVITY_AIDED,                         // byte result 0/1
    QCAMERA3_AR_END
};

// QCAMERA3_VIDEO_HDR_MODE
typedef enum camera_metadata_enum_android_video_hdr_mode {
    QCAMERA3_VIDEO_HDR_MODE_OFF,
    QCAMERA3_VIDEO_HDR_MODE_ON
} camera_metadata_enum_android_video_hdr_mode_t;

// QCAMERA3_IR_MODE
typedef enum camera_metadata_enum_android_ir_mode {
    QCAMERA3_IR_MODE_OFF,
    QCAMERA3_IR_MODE_ON,
    QCAMERA3_IR_MODE_AUTO
} camera_metadata_enum_android_ir_mode_t;

// QCAMERA3_INSTANCE_AEC_MODE
typedef enum qcamera3_ext_instant_aec_mode {
    QCAMERA3_INSTANT_AEC_NORMAL_CONVERGENCE,
    QCAMERA3_INSTANT_AEC_AGGRESSIVE_CONVERGENCE,
    QCAMERA3_INSTANT_AEC_FAST_CONVERGENCE
} qcamera3_ext_instant_aec_mode_t;

// QCAMERA3_HISTOGRAM_MODE
typedef enum qcamera3_histogram_mode {
    QCAMERA3_HISTOGRAM_MODE_OFF,
    QCAMERA3_HISTOGRAM_MODE_ON
} histogram_mode;

// QCAMERA3_BINNING_CORRECTION_MODE
typedef enum camera_metadata_enum_android_binning_correction_mode {
    QCAMERA3_BINNING_CORRECTION_MODE_OFF,
    QCAMERA3_BINNING_CORRECTION_MODE_ON
} camera_metadata_enum_android_binning_correction_mode_t;

// QCAMERA3_EXPOSURE_METER
typedef enum qcamera3_ext_exposure_meter_mode {
    QCAMERA3_EXP_METER_MODE_FRAME_AVERAGE,
    QCAMERA3_EXP_METER_MODE_CENTER_WEIGHTED,
    QCAMERA3_EXP_METER_MODE_SPOT_METERING,
    QCAMERA3_EXP_METER_MODE_SMART_METERING,
    QCAMERA3_EXP_METER_MODE_USER_METERING,
    QCAMERA3_EXP_METER_MODE_SPOT_METERING_ADV,
    QCAMERA3_EXP_METER_MODE_CENTER_WEIGHTED_ADV
} qcamera3_ext_exposure_meter_mode_t;

// QCAMERA3_ISO_EXP_PRIORITY
typedef enum qcamera3_ext_iso_mode {
    QCAMERA3_ISO_MODE_AUTO,
    QCAMERA3_ISO_MODE_DEBLUR,
    QCAMERA3_ISO_MODE_100,
    QCAMERA3_ISO_MODE_200,
    QCAMERA3_ISO_MODE_400,
    QCAMERA3_ISO_MODE_800,
    QCAMERA3_ISO_MODE_1600,
    QCAMERA3_ISO_MODE_3200,
} qcamera3_ext_iso_mode_t;

typedef enum {
    /* RAW only mode for devices that will simultaneously use more than two
     * cameras. This is required because by default the HAL will start support
     * stream along with RAW stream. This will allocate resources which
     * otherwise can be used for another camera. Using this mode indicates HAL
     * that the additional functionality provided by support stream is not
     * needed.
     * This mode has following limitations:
     *    - Back end 3A, Face Detect or any additional functionality depending
     *   on image/sensor statistics and YUV streams will be disabled.
     */
    QCAMERA3_VENDOR_STREAM_CONFIGURATION_RAW_ONLY_MODE = 0x8000,

    /* Disables all support streams for non raw mode configurations. */
    QCAMERA3_VENDOR_STREAM_CONFIGURATION_DISABLE_SUPPORT_STREAMS = 0x8001,

    /*YUV ZSL*/
    QCAMERA3_VENDOR_STREAM_CONFIGURATION_YUV_ZSL_MODE = 0x8002,
    /* Controlling Postprocessing of YUV stream from App. This is required to
     * reduce the performance overhead while multiple callback streams are running
     * by disabling postprocessing which improves power, performance.
     * When App notifies this mode to HAL, HAL doesnt link CPP with any stream
     * This mode has following limitations:
     *    - All YUV stream under this mode wont be connecting CPP
     *    - Postprocessing cant be done.
     */
    QCAMERA3_VENDOR_STREAM_CONFIGURATION_PP_DISABLED_MODE = 0x8004
} QCamera3VendorStreamConfiguration;

class QCamera3VendorTags {

public:
    static void get_vendor_tag_ops(vendor_tag_ops_t* ops);
    // Call before any characteristics are built; the HIDL provider registers
    // its own descriptor later and replaces this one.
    static void registerWithMetadata();
    static int get_tag_count(
            const vendor_tag_ops_t *ops);
    static void get_all_tags(
            const vendor_tag_ops_t *ops,
            uint32_t *tag_array);
    static const char* get_section_name(
            const vendor_tag_ops_t *ops,
            uint32_t tag);
    static const char* get_tag_name(
            const vendor_tag_ops_t *ops,
            uint32_t tag);
    static int get_tag_type(
            const vendor_tag_ops_t *ops,
            uint32_t tag);

    static const vendor_tag_ops_t *Ops;
};

}; // namespace qcamera

#endif /* __QCAMERA3VENDORTAGS_H__ */
