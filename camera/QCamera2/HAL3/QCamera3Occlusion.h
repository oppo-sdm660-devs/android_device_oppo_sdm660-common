/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_OCCLUSION_H
#define QCAMERA3_OCCLUSION_H

#include "QCamera3HdrAlgo.h"

namespace qcamera {

// Camera occlusion (lens covered/blocked) detection stage.
//
// The stock R11/R11s core is libOccluDetect.so (exports stInitialize,
// camOcclusionDetect, stRelease; DT_NEEDED libOpenCL.so). It is dlopen'ed here
// with its real symbol names so the port can report whether the core exists on
// a unit, but its entry point is NOT called: the only caller in the stock HAL
// (libcamera_core_hwi.so) invokes camOcclusionDetect with three register
// arguments on ARM32 - a pointer to an opaque 16-byte stack structure plus two
// words, with the return value ignored and master/slave (main + auxiliary
// camera) results carried out of band - and calls it from the dual-camera ASD
// path, and the library needs an OpenCL context the camera HAL does not create.
// Guessing that ABI from a stripped ELF would risk the whole camera process, so
// this stage runs a documented native detector instead and reports
// Result::coreStatus = kCorePresentNotCalled. capability is never advertised
// from a core that is not actually driven.
//
// This detector is analysis-only: it produces no image. It must never fire on a
// plausibly dark scene without evidence, so a dark frame that still carries
// sensor noise is explicitly declared plausible (REASON_DARK_SCENE_PLAUSIBLE)
// and its confidence is capped below the latch threshold.
class QCamera3Occlusion {
public:
    using Image = QCamera3HdrAlgo::Image;

    enum Path : int32_t { PATH_NATIVE = 0, PATH_CORE = 1 };
    enum CoreStatus : int32_t {
        CORE_ABSENT = 0,             // libOccluDetect.so or a symbol missing
        CORE_PRESENT_NOT_CALLED = 1, // resolved, ABI unverifiable: not called
    };
    enum Reason : int32_t {
        REASON_CLEAR = 0,              // evidence does not support occlusion
        REASON_OCCLUDED_FLAT_BRIGHT = 1,
        REASON_OCCLUDED_FLAT_DARK = 2,
        REASON_DARK_SCENE_PLAUSIBLE = 3,
        REASON_STRUCTURE = 4,
        REASON_LOW_CONFIDENCE = 5,
        REASON_INVALID = 6,
        REASON_CANCELLED = 7,
    };

    struct Options {
        int32_t threshold = 70;         // 1..100 confidence that starts latching
        int32_t releaseThreshold = 40;  // 0..threshold confidence that releases
        int32_t latchFrames = 3;        // 1..kMaxHysteresisFrames consecutive
        int32_t releaseFrames = 3;      // 1..kMaxHysteresisFrames consecutive
        uint32_t frameNumber = 0;
        static bool valid(const Options &o);
    };

    struct Result {
        int32_t status = 0;       // 0 decision produced; negative errno otherwise
        int32_t applied = 0;      // 1 when this call updated the decision state
        uint32_t frameNumber = 0;
        // detail[0] = occluded, detail[1] = confidence, detail[2] = reason,
        // detail[3] = path.
        int32_t detail[4] = {};

        int32_t occluded = 0;     // latched decision after this frame
        int32_t confidence = 0;   // instantaneous evidence score, 0..100
        int32_t reason = REASON_CLEAR;
        int32_t path = PATH_NATIVE;
        int32_t coreStatus = CORE_ABSENT;
        int32_t latchCount = 0, releaseCount = 0;
        // Real statistics the decision was made on (documented in the header):
        // meanLuma 0..255, lowFrequencyVarMilli = variance of the 32x32 block
        // means in 1/1000 luma^2, chromaCastMilli = mean |chroma-128| in 1/1000
        // luma levels, pixelStdMilli = per-pixel standard deviation in 1/1000.
        int32_t meanLuma = 0;
        int32_t lowFrequencyVarMilli = 0;
        int32_t chromaCastMilli = 0;
        int32_t pixelStdMilli = 0;
        // Per-pixel activity score from pixelStdMilli, 0..1000. A high value is
        // real image texture and directly suppresses the occlusion score.
        int32_t activityMilli = 0;
        int32_t darkPlausible = 0;
    };

    QCamera3Occlusion();
    ~QCamera3Occlusion();
    static const char *name();          // "Occlusion"
    static bool isAvailable();          // native detector: always available
    static bool stockCoreAvailable();   // libOccluDetect.so + all three names
    static bool supports(uint32_t width, uint32_t height);
    static bool validOptions(const Options &o);
    // Analysis-only; never writes an image. Transactional: on a negative status
    // the hysteresis state is unchanged.
    int32_t analyse(const Image &source, const Options &options, Result &result,
            const std::atomic<bool> *cancelled = nullptr);
    // Clears the latched decision and the hysteresis counters.
    void reset();
    size_t samples() const;

    static constexpr int32_t kMaxHysteresisFrames = 30;
    static constexpr uint32_t kMaxDimension = 4096;
    // Evidence constants, all in the units used by Result.
    static constexpr int32_t kFlatVarMilli = 4000;    // 4.0 luma^2 block variance
    static constexpr int32_t kDarkLuma = 24;          // plausibly dark mean luma
    static constexpr int32_t kDarkNoiseStdMilli = 1500; // 1.5 luma noise floor
    static constexpr int32_t kDarkConfidenceCap = 25; // dark-scene confidence cap
    static constexpr int32_t kCastMinMilli = 2000;    // 2.0 mean chroma deviation
    static constexpr int32_t kCastScaleMilli = 10000; // 10.0 saturation scale
    static constexpr int32_t kActivityFloorMilli = 3000;  // 3.0 luma noise floor
    static constexpr int32_t kActivityScaleMilli = 12000; // 12.0 luma = full texture

private:
    struct Impl;
    Impl *mImpl;
    QCamera3Occlusion(const QCamera3Occlusion &) = delete;
    QCamera3Occlusion &operator=(const QCamera3Occlusion &) = delete;
};

} // namespace qcamera
#endif
