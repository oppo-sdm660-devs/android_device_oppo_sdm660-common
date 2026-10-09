/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_AUTO_CAPTURE_POLICY_H
#define QCAMERA3_AUTO_CAPTURE_POLICY_H

#include "QCamera3NativeAlgorithms.h"
#include <stdint.h>

namespace qcamera {

// Automatic HDR / low-light trigger policy for the ordinary AUTO path.
//
// It is a pure decision function over REAL frame statistics. It never creates a
// still mode of its own: it only selects an existing native mode
// (OPPO_NATIVE_HDR or OPPO_NATIVE_LLHDR) or single shot. Every statistic is
// optional and carries an explicit absent value; a statistic that is absent or
// malformed keeps single shot and is never guessed, interpolated or synthesised
// from another field. There is no camera, no metadata dependency and no
// allocation; the whole state is a bounded set of counters.
//
// The stock HAL1 path decided from the ASD scene label and its own parameter
// record. This policy does not read the semantic classifier and does not reuse
// stock thresholds; those were not recovered from the binaries, so new named
// thresholds are used and documented in device-tree-nonsource-20261017/camera/docs/AUTO_CAPTURE_POLICY.md.
class QCamera3AutoCapturePolicy {
public:
    static constexpr uint32_t kHistogramBins = 256;
    // Bounded, real-statistics limits. Values above these are malformed input.
    static constexpr int64_t kMaxExposureTimeNs = 10000000000LL;   // 10 s
    static constexpr int32_t kMaxIso = 1024000;
    static constexpr int32_t kMaxLuxMilli = 100000000;             // 100000 lx
    static constexpr int32_t kMaxMotionMilli = 1000;
    static constexpr int32_t kMaxClippedMilli = 1000;              // per-mille of pixels

    enum Mode {
        kModeSingleShot = OPPO_NATIVE_OFF,   // 0: no extra mode
        kModeHdr = OPPO_NATIVE_HDR,          // 1: existing bracketed HDR
        kModeNight = OPPO_NATIVE_LLHDR,      // 2: existing low-light HDR
    };

    enum Reason {
        kReasonKeep = 0,          // below every entry threshold
        kReasonDisabled = 1,      // both automatic triggers disabled by the request
        kReasonMissingStats = 2,  // the required real statistic is absent
        kReasonInvalidStats = 3,  // a present statistic is malformed / out of range
        kReasonLowLight = 4,      // night score at or above the entry threshold
        kReasonBacklight = 5,     // HDR triggered by real highlight clipping
        kReasonDynamicRange = 6,  // HDR triggered by the real histogram range
        kReasonShadowDetail = 7,  // HDR triggered by real shadow clipping
        kReasonHolding = 8,       // current mode held, exit hysteresis not met
        kReasonExit = 9,          // current mode just released
        kReasonCooldown = 10,     // re-entry blocked by the post-exit cooldown
        kReasonMotion = 11,       // motion blocks a new entry
    };

    // Real per-frame statistics. Absent fields keep their documented sentinel;
    // the policy treats them as unavailable and never invents a value.
    struct Stats {
        // Y histogram of the *statistics* frame, 256 bins, bin i = luma i.
        const uint32_t *lumaHistogram = nullptr;
        uint32_t histogramTotal = 0;       // must equal the sum of the 256 bins
        int64_t exposureTimeNs = -1;       // ANDROID_SENSOR_EXPOSURE_TIME
        int32_t iso = -1;                  // ANDROID_SENSOR_SENSITIVITY
        int32_t luxMilli = -1;             // real lux * 1000, -1 when unavailable
        int32_t motionMilli = -1;          // 0..1000 real motion estimate, -1 absent
        int32_t highlightClippedMilli = -1;// 0..1000 of pixels at luma >= 250
        int32_t shadowClippedMilli = -1;   // 0..1000 of pixels at luma <= 5
        bool hasLux() const { return luxMilli >= 0; }
        bool hasExposureIndex() const { return exposureTimeNs > 0 && iso > 0; }
        bool hasHistogram() const { return lumaHistogram && histogramTotal; }
    };

    // Request/capability side. Defaults are the documented named thresholds.
    struct Config {
        int32_t hdrEnable = 1;                 // autoHdrEnable
        int32_t nightEnable = 1;               // autoNightEnable
        // Night: real lux when available.
        int32_t nightEnterLuxMilli = 30000;    // 30 lx
        int32_t nightExitLuxMilli = 80000;     // 80 lx
        // Night: relative darkness index when only real exposure/ISO exist.
        // index = (exposureTimeNs / 1e6) * iso / 10; it is NOT a lux value.
        int32_t nightEnterIndex = 2500;
        int32_t nightExitIndex = 1000;
        // HDR: real highlight clipping, per-mille of pixels.
        int32_t hdrClipEnterMilli = 50;        // 5 %
        int32_t hdrClipExitMilli = 20;         // 2 %
        // HDR: real shadow clipping, per-mille of pixels.
        int32_t hdrShadowEnterMilli = 120;     // 12 %
        int32_t hdrShadowExitMilli = 60;       // 6 %
        // HDR: real 5th..95th percentile luma spread of the histogram.
        int32_t hdrRangeEnter = 200;
        int32_t hdrRangeExit = 120;
        // HDR: real mean/median separation (backlight), luma levels.
        int32_t hdrBacklightEnter = 40;
        int32_t hdrBacklightExit = 20;
        int32_t motionHoldMilli = 700;         // >= blocks a new entry
        int32_t enterFrames = 3;               // consecutive frames above entry
        int32_t exitFrames = 5;                // consecutive frames below exit
        int32_t cooldownFrames = 30;           // re-entry lock after an exit
        static bool valid(const Config &c);
    };

    struct Decision {
        int32_t mode = kModeSingleShot;  // existing native mode to run
        int32_t reason = kReasonKeep;
        int32_t triggerScore = 0;        // 0..1000 score of the selected mode
        int32_t nightScore = 0;          // 0..1000, diagnostics only
        int32_t hdrScore = 0;            // 0..1000, diagnostics only
        int32_t heldFrames = 0;          // frames the returned mode has been held
        int32_t usedRealLux = 0;         // 1 only when a real lux value was used
        bool triggers() const { return mode != kModeSingleShot; }
    };

    // Bounded state: three counters, one mode, one frame number. No allocation.
    struct State {
        int32_t mode = kModeSingleShot;
        int32_t candidate = kModeSingleShot;
        int32_t candidateFrames = 0;
        int32_t holdFrames = 0;
        int32_t heldFrames = 0;
        int32_t cooldown[3] = {0, 0, 0};
        uint32_t lastFrame = 0;
        int32_t lastReason = kReasonKeep;   // reason reported by the last decision
        int32_t entryReason = kReasonKeep;  // reason the held mode was entered with
    };

    // Malformed present statistics (never absent ones) return false.
    static bool validStats(const Stats &stats);
    // Real histogram percentiles in luma levels. Returns false without a
    // consistent histogram (null, zero total or bin sum != histogramTotal).
    static bool histogramPercentiles(const Stats &stats, int32_t &p5, int32_t &p50, int32_t &p95);
    // 0..1000 real-histogram mean/median separation.
    static int32_t backlightScore(const Stats &stats, const Config &config);
    // 0..1000 low-light score. `usedRealLux` reports which input was used.
    static int32_t nightScore(const Stats &stats, const Config &config, bool *usedRealLux);
    // 0..1000 HDR score; `reason` names the strongest real contributor.
    static int32_t hdrScore(const Stats &stats, const Config &config, int32_t &reason);
    // Documented relative darkness index, or -1 when exposure/ISO are absent.
    static int64_t darknessIndex(const Stats &stats);

    void reset();
    // One call per decided frame, monotonic frame numbers. A non-monotonic
    // frame number resets the state (deterministic under log replay).
    Decision decide(const Stats &stats, const Config &config, uint32_t frameNumber);
    const State &state() const { return mState; }

private:
    State mState;
};

} // namespace qcamera
#endif
