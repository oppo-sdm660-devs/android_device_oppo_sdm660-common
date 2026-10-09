/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_AUTO_CAPTURE_POLICY_H
#define QCAMERA3_AUTO_CAPTURE_POLICY_H

#include "QCamera3NativeAlgorithms.h"
#include <stdint.h>

namespace qcamera {

// Automatic HDR / low-light trigger policy for the ordinary AUTO path.
class QCamera3AutoCapturePolicy {
public:
    static constexpr uint32_t kHistogramBins = 256;
    // Input limits; larger values are malformed.
    static constexpr int64_t kMaxExposureTimeNs = 10000000000LL;   // 10 s
    static constexpr int32_t kMaxIso = 1024000;
    static constexpr int32_t kMaxLuxMilli = 100000000;             // 100000 lx
    static constexpr int32_t kMaxMotionMilli = 1000;
    static constexpr int32_t kMaxClippedMilli = 1000;              // per-mille

    enum Mode {
        kModeSingleShot = OPPO_NATIVE_OFF,
        kModeHdr = OPPO_NATIVE_HDR,
        kModeNight = OPPO_NATIVE_LLHDR,
    };

    enum Reason {
        kReasonKeep = 0,
        kReasonDisabled = 1,
        kReasonMissingStats = 2,
        kReasonInvalidStats = 3,
        kReasonLowLight = 4,
        kReasonBacklight = 5,
        kReasonDynamicRange = 6,
        kReasonShadowDetail = 7,
        kReasonHolding = 8,
        kReasonExit = 9,
        kReasonCooldown = 10,
        kReasonMotion = 11,
    };

    // Real per-frame statistics; -1 (or a null histogram) means unavailable.
    struct Stats {
        const uint32_t *lumaHistogram = nullptr;
        uint32_t histogramTotal = 0;
        int64_t exposureTimeNs = -1;       // ANDROID_SENSOR_EXPOSURE_TIME
        int32_t iso = -1;                  // ANDROID_SENSOR_SENSITIVITY
        int32_t luxMilli = -1;             // lux * 1000
        int32_t motionMilli = -1;          // 0..1000
        int32_t highlightClippedMilli = -1;// 0..1000 pixels at luma >= 250
        int32_t shadowClippedMilli = -1;   // 0..1000 pixels at luma <= 5
        bool hasLux() const { return luxMilli >= 0; }
        bool hasExposureIndex() const { return exposureTimeNs > 0 && iso > 0; }
        bool hasHistogram() const { return lumaHistogram && histogramTotal; }
    };

    struct Config {
        int32_t hdrEnable = 1;
        int32_t nightEnable = 1;
        int32_t nightEnterLuxMilli = 30000;    // 30 lx
        int32_t nightExitLuxMilli = 80000;     // 80 lx
        // Darkness index = (exposureTimeNs / 1e6) * iso / 10, not a lux value.
        int32_t nightEnterIndex = 2500;
        int32_t nightExitIndex = 1000;
        int32_t hdrClipEnterMilli = 50;        // 5 %
        int32_t hdrClipExitMilli = 20;         // 2 %
        int32_t hdrShadowEnterMilli = 120;     // 12 %
        int32_t hdrShadowExitMilli = 60;       // 6 %
        int32_t hdrRangeEnter = 200;
        int32_t hdrRangeExit = 120;
        int32_t hdrBacklightEnter = 40;
        int32_t hdrBacklightExit = 20;
        int32_t motionHoldMilli = 700;
        int32_t enterFrames = 3;
        int32_t exitFrames = 5;
        int32_t cooldownFrames = 30;
        static bool valid(const Config &c);
    };

    struct Decision {
        int32_t mode = kModeSingleShot;
        int32_t reason = kReasonKeep;
        int32_t triggerScore = 0;        // 0..1000
        int32_t nightScore = 0;          // 0..1000, diagnostics
        int32_t hdrScore = 0;            // 0..1000, diagnostics
        int32_t heldFrames = 0;
        int32_t usedRealLux = 0;
        bool triggers() const { return mode != kModeSingleShot; }
    };

    struct State {
        int32_t mode = kModeSingleShot;
        int32_t candidate = kModeSingleShot;
        int32_t candidateFrames = 0;
        int32_t holdFrames = 0;
        int32_t heldFrames = 0;
        int32_t cooldown[3] = {0, 0, 0};
        uint32_t lastFrame = 0;
        int32_t lastReason = kReasonKeep;
        int32_t entryReason = kReasonKeep;
    };

    static bool validStats(const Stats &stats);
    // Returns false unless the histogram is non-empty and consistent.
    static bool histogramPercentiles(const Stats &stats, int32_t &p5, int32_t &p50, int32_t &p95);
    static int32_t backlightScore(const Stats &stats, const Config &config);
    static int32_t nightScore(const Stats &stats, const Config &config, bool *usedRealLux);
    static int32_t hdrScore(const Stats &stats, const Config &config, int32_t &reason);
    // Relative darkness index, or -1 without exposure/ISO.
    static int64_t darknessIndex(const Stats &stats);

    void reset();
    // One call per decided frame; a non-monotonic frame number resets the state.
    Decision decide(const Stats &stats, const Config &config, uint32_t frameNumber);
    const State &state() const { return mState; }

private:
    State mState;
};

} // namespace qcamera
#endif
