/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_OCCLUSION_H
#define QCAMERA3_OCCLUSION_H

#include "QCamera3HdrAlgo.h"

namespace qcamera {

// Camera occlusion (lens covered/blocked) detection stage.
// The stock core libOccluDetect.so is probed but never called (ARM32 ABI unverifiable).
class QCamera3Occlusion {
public:
    using Image = QCamera3HdrAlgo::Image;

    enum Path : int32_t { PATH_NATIVE = 0, PATH_CORE = 1 };
    enum CoreStatus : int32_t {
        CORE_ABSENT = 0,             // libOccluDetect.so or a symbol missing
        CORE_PRESENT_NOT_CALLED = 1, // resolved, ABI unverifiable: not called
    };
    enum Reason : int32_t {
        REASON_CLEAR = 0,
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
        int32_t applied = 0;
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
        // meanLuma 0..255; lowFrequencyVarMilli = block-mean variance in 1/1000
        // luma^2; chromaCastMilli = mean |chroma-128| / 1000; pixelStdMilli =
        // per-pixel standard deviation / 1000.
        int32_t meanLuma = 0;
        int32_t lowFrequencyVarMilli = 0;
        int32_t chromaCastMilli = 0;
        int32_t pixelStdMilli = 0;
        // Pixel-activity score 0..1000; high real texture suppresses occlusion.
        int32_t activityMilli = 0;
        int32_t darkPlausible = 0;
    };

    QCamera3Occlusion();
    ~QCamera3Occlusion();
    static const char *name();
    static bool isAvailable();          // native detector: always available
    static bool stockCoreAvailable();   // libOccluDetect.so + all three names
    static bool supports(uint32_t width, uint32_t height);
    static bool validOptions(const Options &o);
    // Analysis-only; on a negative status the hysteresis state is unchanged.
    int32_t analyse(const Image &source, const Options &options, Result &result,
            const std::atomic<bool> *cancelled = nullptr);
    void reset();
    size_t samples() const;

    static constexpr int32_t kMaxHysteresisFrames = 30;
    static constexpr uint32_t kMaxDimension = 4096;
    // Threshold constants, in the units used by Result.
    static constexpr int32_t kFlatVarMilli = 4000;        // 4.0 luma^2 block variance
    static constexpr int32_t kDarkLuma = 24;              // plausibly dark mean luma
    static constexpr int32_t kDarkNoiseStdMilli = 1500;   // 1.5 luma noise floor
    static constexpr int32_t kDarkConfidenceCap = 25;     // dark-scene confidence cap
    static constexpr int32_t kCastMinMilli = 2000;        // 2.0 mean chroma deviation
    static constexpr int32_t kCastScaleMilli = 10000;     // 10.0 saturation scale
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
