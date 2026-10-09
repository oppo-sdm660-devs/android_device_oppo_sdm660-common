/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3AutoCapturePolicy.h"

namespace qcamera {
namespace {
constexpr uint32_t kNoFrame = UINT32_MAX;
constexpr int32_t kEnterScore = 1000;   // at or above the named entry threshold
int32_t clampMilli(int64_t value) {
    if (value <= 0) return 0;
    return value >= 1000 ? 1000 : int32_t(value);
}
// Linear ramp from 0 at `exitValue` to 1000 at `enterValue`. Callers always pass
// thresholds in the direction that makes exitValue the permissive end, so a
// score of zero means "the real statistic is at or beyond the exit value".
int32_t ramp(int64_t value, int64_t enterValue, int64_t exitValue) {
    if (value >= enterValue) return 1000;
    if (value <= exitValue) return 0;
    return clampMilli((value - exitValue) * 1000 / (enterValue - exitValue));
}
void consider(int32_t score, int32_t reason, int32_t &bestScore, int32_t &bestReason) {
    if (score > bestScore) {
        bestScore = score;
        bestReason = reason;
    }
}
} // namespace

bool QCamera3AutoCapturePolicy::Config::valid(const Config &c) {
    if (c.hdrEnable != 0 && c.hdrEnable != 1) return false;
    if (c.nightEnable != 0 && c.nightEnable != 1) return false;
    if (c.nightEnterLuxMilli <= 0 || c.nightExitLuxMilli <= c.nightEnterLuxMilli) return false;
    if (c.nightEnterIndex <= 0 || c.nightExitIndex >= c.nightEnterIndex) return false;
    if (c.hdrClipEnterMilli <= 0 || c.hdrClipEnterMilli > kMaxClippedMilli) return false;
    if (c.hdrClipExitMilli < 0 || c.hdrClipExitMilli >= c.hdrClipEnterMilli) return false;
    if (c.hdrShadowEnterMilli <= 0 || c.hdrShadowEnterMilli > kMaxClippedMilli) return false;
    if (c.hdrShadowExitMilli < 0 || c.hdrShadowExitMilli >= c.hdrShadowEnterMilli) return false;
    if (c.hdrRangeEnter <= 0 || c.hdrRangeEnter > 255 || c.hdrRangeExit < 0 ||
            c.hdrRangeExit >= c.hdrRangeEnter) return false;
    if (c.hdrBacklightEnter <= 0 || c.hdrBacklightEnter > 255 ||
            c.hdrBacklightExit < 0 || c.hdrBacklightExit >= c.hdrBacklightEnter) return false;
    if (c.motionHoldMilli < 0 || c.motionHoldMilli > kMaxMotionMilli) return false;
    if (c.enterFrames < 1 || c.enterFrames > 600) return false;
    if (c.exitFrames < 1 || c.exitFrames > 600) return false;
    if (c.cooldownFrames < 0 || c.cooldownFrames > 100000) return false;
    return true;
}

bool QCamera3AutoCapturePolicy::validStats(const Stats &s) {
    if (s.lumaHistogram) {
        if (!s.histogramTotal) return false;
        uint64_t sum = 0;
        for (uint32_t i = 0; i < kHistogramBins; ++i) {
            sum += s.lumaHistogram[i];
            if (sum > (UINT64_C(1) << 40)) return false;  // absurd bin content
        }
        if (sum != s.histogramTotal) return false;
    } else if (s.histogramTotal) {
        return false;  // a total without bins is not a usable statistic
    }
    if (s.exposureTimeNs != -1 &&
            (s.exposureTimeNs <= 0 || s.exposureTimeNs > kMaxExposureTimeNs)) return false;
    if (s.iso != -1 && (s.iso <= 0 || s.iso > kMaxIso)) return false;
    if (s.luxMilli != -1 && (s.luxMilli < 0 || s.luxMilli > kMaxLuxMilli)) return false;
    if (s.motionMilli != -1 &&
            (s.motionMilli < 0 || s.motionMilli > kMaxMotionMilli)) return false;
    if (s.highlightClippedMilli != -1 &&
            (s.highlightClippedMilli < 0 || s.highlightClippedMilli > kMaxClippedMilli))
        return false;
    if (s.shadowClippedMilli != -1 &&
            (s.shadowClippedMilli < 0 || s.shadowClippedMilli > kMaxClippedMilli))
        return false;
    return true;
}

bool QCamera3AutoCapturePolicy::histogramPercentiles(const Stats &s,
        int32_t &p5, int32_t &p50, int32_t &p95) {
    p5 = p50 = p95 = 0;
    if (!s.lumaHistogram || !s.histogramTotal) return false;
    uint64_t total = 0;
    for (uint32_t i = 0; i < kHistogramBins; ++i) total += s.lumaHistogram[i];
    if (!total || total != s.histogramTotal) return false;
    // Integer per-mille ranks; the bin index is reported without interpolation.
    const uint64_t rank5 = (total * 5 + 50) / 100;
    const uint64_t rank50 = (total + 1) / 2;
    const uint64_t rank95 = (total * 95 + 50) / 100;
    uint64_t cumulative = 0;
    bool have5 = false, have50 = false;
    for (uint32_t i = 0; i < kHistogramBins; ++i) {
        cumulative += s.lumaHistogram[i];
        if (!have5 && cumulative >= rank5) { p5 = int32_t(i); have5 = true; }
        if (!have50 && cumulative >= rank50) { p50 = int32_t(i); have50 = true; }
        if (cumulative >= rank95) { p95 = int32_t(i); break; }
    }
    if (!have5 || !have50) return false;
    return true;
}

int32_t QCamera3AutoCapturePolicy::backlightScore(const Stats &s, const Config &c) {
    int32_t p5 = 0, p50 = 0, p95 = 0;
    if (!histogramPercentiles(s, p5, p50, p95)) return 0;
    uint64_t weighted = 0;
    for (uint32_t i = 0; i < kHistogramBins; ++i) weighted += uint64_t(i) * s.lumaHistogram[i];
    const int64_t mean = int64_t(weighted / s.histogramTotal);
    const int64_t separation = mean > p50 ? mean - p50 : int64_t(p50) - mean;
    return ramp(separation, c.hdrBacklightEnter, c.hdrBacklightExit);
}

int64_t QCamera3AutoCapturePolicy::darknessIndex(const Stats &s) {
    if (!s.hasExposureIndex()) return -1;
    // Relative darkness index in ms * ISO / 10. It is a documented exposure
    // product, not a lux measurement and not a photometric conversion.
    const int64_t milliseconds = s.exposureTimeNs / 1000000;
    return milliseconds * int64_t(s.iso) / 10;
}

int32_t QCamera3AutoCapturePolicy::nightScore(const Stats &s, const Config &c,
        bool *usedRealLux) {
    if (usedRealLux) *usedRealLux = false;
    if (s.hasLux()) {
        if (usedRealLux) *usedRealLux = true;
        // 1000 at or below nightEnterLuxMilli, 0 at or above nightExitLuxMilli.
        return ramp(int64_t(c.nightExitLuxMilli) - s.luxMilli,
                int64_t(c.nightExitLuxMilli) - c.nightEnterLuxMilli, 0);
    }
    const int64_t index = darknessIndex(s);
    if (index < 0) return 0;
    return ramp(index, c.nightEnterIndex, c.nightExitIndex);
}

int32_t QCamera3AutoCapturePolicy::hdrScore(const Stats &s, const Config &c, int32_t &reason) {
    int32_t best = 0;
    reason = kReasonKeep;
    if (s.highlightClippedMilli >= 0) {
        consider(ramp(s.highlightClippedMilli, c.hdrClipEnterMilli, c.hdrClipExitMilli),
                kReasonBacklight, best, reason);
    }
    if (s.shadowClippedMilli >= 0) {
        consider(ramp(s.shadowClippedMilli, c.hdrShadowEnterMilli, c.hdrShadowExitMilli),
                kReasonShadowDetail, best, reason);
    }
    if (s.hasHistogram()) {
        int32_t p5 = 0, p50 = 0, p95 = 0;
        if (histogramPercentiles(s, p5, p50, p95)) {
            consider(ramp(int64_t(p95) - p5, c.hdrRangeEnter, c.hdrRangeExit),
                    kReasonDynamicRange, best, reason);
            consider(backlightScore(s, c), kReasonBacklight, best, reason);
        }
    }
    return best;
}

void QCamera3AutoCapturePolicy::reset() { mState = State(); }

QCamera3AutoCapturePolicy::Decision QCamera3AutoCapturePolicy::decide(
        const Stats &stats, const Config &config, uint32_t frameNumber) {
    Decision decision;
    uint32_t elapsed = 1;
    if (mState.lastFrame != kNoFrame) {
        if (frameNumber <= mState.lastFrame) reset();  // non-monotonic: deterministic restart
        else elapsed = frameNumber - mState.lastFrame;
    }
    if (elapsed > 255) elapsed = 255;
    mState.lastFrame = frameNumber;
    for (int i = 0; i < 3; ++i)
        mState.cooldown[i] = mState.cooldown[i] > int32_t(elapsed) ?
                mState.cooldown[i] - int32_t(elapsed) : 0;

    if (!Config::valid(config) || (!config.hdrEnable && !config.nightEnable)) {
        reset();
        mState.lastFrame = frameNumber;
        decision.reason = kReasonDisabled;
        return decision;
    }
    if (!validStats(stats)) {
        reset();
        mState.lastFrame = frameNumber;
        decision.reason = kReasonInvalidStats;
        return decision;
    }
    const bool nightAvailable = config.nightEnable &&
            (stats.hasLux() || stats.hasExposureIndex());
    const bool hdrAvailable = config.hdrEnable &&
            (stats.highlightClippedMilli >= 0 || stats.shadowClippedMilli >= 0 ||
             stats.hasHistogram());
    bool usedRealLux = false;
    const int32_t night = nightAvailable ? nightScore(stats, config, &usedRealLux) : 0;
    int32_t hdrReason = kReasonKeep;
    const int32_t hdr = hdrAvailable ? hdrScore(stats, config, hdrReason) : 0;
    decision.nightScore = night;
    decision.hdrScore = hdr;
    decision.usedRealLux = usedRealLux ? 1 : 0;

    if (!nightAvailable && !hdrAvailable) {
        reset();
        mState.lastFrame = frameNumber;
        decision.reason = kReasonMissingStats;
        return decision;
    }

    int32_t candidate = kModeSingleShot;
    int32_t candidateReason = kReasonKeep;
    int32_t candidateScore = 0;
    if (nightAvailable && night >= kEnterScore) {
        candidate = kModeNight;
        candidateReason = kReasonLowLight;
        candidateScore = night;
    } else if (hdrAvailable && hdr >= kEnterScore) {
        candidate = kModeHdr;
        candidateReason = hdrReason;
        candidateScore = hdr;
    }
    const bool motionBlocked = stats.motionMilli >= 0 &&
            stats.motionMilli >= config.motionHoldMilli;

    if (mState.mode != kModeSingleShot) {
        // Hysteresis. The ramps encode the documented exit values: a score of
        // zero means the real statistic crossed the exit value. The mode is
        // released only after exitFrames consecutive zero-score frames.
        const int32_t currentScore = mState.mode == kModeNight ? night :
                mState.mode == kModeHdr ? hdr : 0;
        const bool currentAvailable = mState.mode == kModeNight ? nightAvailable : hdrAvailable;
        if (currentAvailable && currentScore > 0) {
            mState.holdFrames = 0;
            ++mState.heldFrames;
            decision.mode = mState.mode;
            decision.reason = currentScore >= kEnterScore ? mState.entryReason :
                    kReasonHolding;
            decision.triggerScore = currentScore;
            decision.heldFrames = mState.heldFrames;
            return decision;
        }
        ++mState.holdFrames;
        ++mState.heldFrames;
        if (mState.holdFrames < config.exitFrames) {
            decision.mode = mState.mode;
            decision.reason = kReasonHolding;
            decision.triggerScore = currentScore;
            decision.heldFrames = mState.heldFrames;
            return decision;
        }
        mState.cooldown[mState.mode] = config.cooldownFrames;
        mState.mode = kModeSingleShot;
        mState.holdFrames = 0;
        mState.heldFrames = 0;
        mState.candidate = kModeSingleShot;
        mState.candidateFrames = 0;
        mState.lastReason = kReasonExit;
        mState.entryReason = kReasonKeep;
        decision.reason = kReasonExit;
        decision.triggerScore = currentScore;
        return decision;
    }

    mState.holdFrames = 0;
    mState.heldFrames = 0;
    if (candidate == kModeSingleShot) {
        mState.candidate = kModeSingleShot;
        mState.candidateFrames = 0;
        decision.reason = motionBlocked ? kReasonMotion : kReasonKeep;
        mState.lastReason = decision.reason;
        return decision;
    }
    if (mState.cooldown[candidate] > 0) {
        decision.reason = kReasonCooldown;
        decision.triggerScore = candidateScore;
        mState.lastReason = decision.reason;
        return decision;
    }
    if (motionBlocked) {
        decision.reason = kReasonMotion;
        decision.triggerScore = candidateScore;
        mState.lastReason = decision.reason;
        return decision;
    }
    if (mState.candidate != candidate) {
        mState.candidate = candidate;
        mState.candidateFrames = 0;
    }
    ++mState.candidateFrames;
    if (mState.candidateFrames < config.enterFrames) {
        decision.reason = kReasonKeep;
        decision.triggerScore = candidateScore;
        mState.lastReason = decision.reason;
        return decision;
    }
    mState.mode = candidate;
    mState.candidate = kModeSingleShot;
    mState.candidateFrames = 0;
    mState.heldFrames = 1;
    mState.lastReason = candidateReason;
    mState.entryReason = candidateReason;
    decision.mode = candidate;
    decision.reason = candidateReason;
    decision.triggerScore = candidateScore;
    decision.heldFrames = 1;
    return decision;
}

} // namespace qcamera
