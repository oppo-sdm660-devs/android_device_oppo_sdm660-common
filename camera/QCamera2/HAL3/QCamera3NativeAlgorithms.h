/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_NATIVE_ALGORITHMS_H
#define QCAMERA3_NATIVE_ALGORITHMS_H
namespace qcamera {
// Per-still request values. Advertisement requires the corresponding core and
// an implemented HAL3 input contract; these are not stock daemon enum values.
enum QCamera3NativeAlgorithm {
    OPPO_NATIVE_OFF = 0,
    OPPO_NATIVE_HDR = 1,
    OPPO_NATIVE_LLHDR = 2,
    OPPO_NATIVE_MFNR = 3,
    OPPO_NATIVE_SUPERPHOTO = 4,
    OPPO_NATIVE_REAR_BEAUTY = 5,
    OPPO_NATIVE_FILTER = 6,
    OPPO_NATIVE_DUAL_BOKEH = 7,
    OPPO_NATIVE_DUAL_RELIGHT = 8,
    OPPO_NATIVE_SCENE = 9,
    // Round 2: HAL3-owned stages. See docs/HAL3_PORT_PLATFORM.md section 2.
    OPPO_NATIVE_FRONT_BEAUTY = 10,
    OPPO_NATIVE_SINGLE_BOKEH = 11,
    OPPO_NATIVE_PICBEST = 12,
    OPPO_NATIVE_IMAGE_ADJUST = 13,
    OPPO_NATIVE_SINGLE_RELIGHT = 14,
    OPPO_NATIVE_DOC = 15,
    OPPO_NATIVE_SCES = 16,
    OPPO_NATIVE_DOUBLE_EXPOSURE = 17,
    OPPO_NATIVE_LONG_EXPOSURE = 18,
    OPPO_NATIVE_AR_TRACK = 19,
    OPPO_NATIVE_REMOSAIC = 20,
};

// Continuous (preview/video) effect identifiers carried by
// org.lineageos.camera.algorithms.continuousAvailableEffects. 1..4 are the
// round-1 effects; 5..9 are owned by the HAL3 stages in this round.
enum QCamera3NativeContinuousEffect {
    OPPO_CONTINUOUS_OFF = 0,
    OPPO_CONTINUOUS_REAR_BEAUTY = 1,
    OPPO_CONTINUOUS_FILTER = 2,
    OPPO_CONTINUOUS_TONE = 3,
    OPPO_CONTINUOUS_SCENE = 4,
    OPPO_CONTINUOUS_FRONT_BEAUTY = 5,
    OPPO_CONTINUOUS_VIDEO_FILTER = 6,
    OPPO_CONTINUOUS_THIRD_PARTY_BEAUTY = 7,
    OPPO_CONTINUOUS_SINGLE_BOKEH_PREVIEW = 8,
    OPPO_CONTINUOUS_AR_TRACK = 9,
};
}
#endif
