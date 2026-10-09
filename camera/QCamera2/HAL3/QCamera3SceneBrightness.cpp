/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3SceneBrightness.h"

#include <errno.h>

namespace qcamera {
namespace {

using Scene = QCamera3SceneBrightness;

bool stopped(const std::atomic<bool> *flag) { return flag && flag->load(); }

int32_t fail(Scene::Result &result, uint32_t frameNumber, int32_t status, int32_t reason,
        int32_t detail1 = 0, int32_t detail2 = 0) {
    Scene::Result local;
    local.frameNumber = frameNumber;
    local.status = status;
    local.detail[0] = reason;
    local.detail[1] = detail1;
    local.detail[2] = detail2;
    result = local;
    return status;
}

int32_t clampInt32(int32_t value, int32_t low, int32_t high) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

// Matches the real classified scene id against the caller's real target table.
// An absent scene class or an empty table never matches (nothing is invented).
bool brightnessTarget(const Scene::Stats &stats, const Scene::Config &config,
        int32_t &brightness) {
    if (stats.sceneClass < 0 || !config.targets || config.targetCount == 0) return false;
    for (uint32_t i = 0; i < config.targetCount; ++i) {
        if (config.targets[i].sceneId != stats.sceneClass) continue;
        int32_t value = clampInt32(config.targets[i].brightness, config.brightnessMin,
                config.brightnessMax);
        // Snap down to the real decoded step grid measured from brightnessMin.
        value = config.brightnessMin +
                ((value - config.brightnessMin) / config.brightnessStep) *
                config.brightnessStep;
        brightness = value;
        return true;
    }
    return false;
}

} // namespace

bool QCamera3SceneBrightness::isAvailable() { return true; }

const char *QCamera3SceneBrightness::name() { return "SceneBrightness"; }

bool QCamera3SceneBrightness::supports(uint32_t width, uint32_t height) {
    return width >= 16 && height >= 16 && width <= 16384 && height <= 16384 &&
            !(width & 1) && !(height & 1);
}

bool QCamera3SceneBrightness::Config::valid(const Config &c) {
    if (c.brightnessMin < -100000 || c.brightnessMax > 100000 ||
            c.brightnessMax <= c.brightnessMin) return false;
    if (c.brightnessStep < 1 || c.brightnessStep > c.brightnessMax - c.brightnessMin)
        return false;
    if (c.targetCount > MAX_SCENE_TARGETS) return false;
    if (c.targetCount > 0 && !c.targets) return false;
    for (uint32_t i = 0; i < c.targetCount && c.targets; ++i) {
        if (c.targets[i].sceneId < 0) return false;
        if (c.targets[i].brightness < c.brightnessMin ||
                c.targets[i].brightness > c.brightnessMax) return false;
    }
    if (c.macroFocusDistanceMilli < -1 || c.macroFocusDistanceMilli > 1000000)
        return false;
    if (c.macroLuxMilli < -1 || c.macroLuxMilli > 100000000) return false;
    if (c.macroMeanLumaMilli < -1 || c.macroMeanLumaMilli > 255000) return false;
    if (c.enterFrames < 1 || c.enterFrames > MAX_ENTER_FRAMES) return false;
    if (c.inactivityFrames < 1 || c.inactivityFrames > MAX_INACTIVITY_FRAMES)
        return false;
    if (c.cooldownFrames < 0 || c.cooldownFrames > MAX_COOLDOWN_FRAMES) return false;
    if (c.motionHoldMilli < 0 || c.motionHoldMilli > 1000) return false;
    return true;
}

bool QCamera3SceneBrightness::validConfig(const Config &config) {
    return Config::valid(config);
}

bool QCamera3SceneBrightness::validStats(const Stats &stats) {
    if (stats.meanLumaMilli < -1 || stats.meanLumaMilli > 255000) return false;
    if (stats.focusDistanceMilli < -1 || stats.focusDistanceMilli > 1000000) return false;
    if (stats.sceneClass < -1 || stats.sceneClass > 100000) return false;
    if (stats.luxMilli < -1 || stats.luxMilli > 100000000) return false;
    if (stats.motionMilli < -1 || stats.motionMilli > 1000) return false;
    if (!stats.lumaHistogram && stats.histogramTotal != 0) return false;
    if (stats.lumaHistogram) {
        if (stats.histogramTotal == 0) return false;
        uint64_t sum = 0;
        for (uint32_t i = 0; i < kHistogramBins; ++i) sum += stats.lumaHistogram[i];
        if (sum != stats.histogramTotal) return false;
    }
    return true;
}

bool QCamera3SceneBrightness::usableStats(const Stats &stats) {
    return stats.lumaHistogram != nullptr || stats.meanLumaMilli >= 0 ||
            stats.focusDistanceMilli >= 0 || stats.sceneClass >= 0 ||
            stats.luxMilli >= 0;
}

bool QCamera3SceneBrightness::histogramMeanLumaMilli(const Stats &stats,
        int32_t &meanMilli) {
    if (!stats.lumaHistogram || stats.histogramTotal == 0) return false;
    uint64_t weighted = 0, total = 0;
    for (uint32_t i = 0; i < kHistogramBins; ++i) {
        weighted += uint64_t(i) * stats.lumaHistogram[i];
        total += stats.lumaHistogram[i];
    }
    if (total == 0 || total != stats.histogramTotal) return false;
    meanMilli = int32_t((weighted * 1000 + total / 2) / total);
    return true;
}

bool QCamera3SceneBrightness::lowLightMacro(const Stats &stats, const Config &config) {
    // No real decoded macro threshold means the detection is off; a threshold is
    // never invented from a scene name or a default distance.
    if (config.macroFocusDistanceMilli < 0) return false;
    if (stats.focusDistanceMilli < 0) return false;
    if (stats.focusDistanceMilli < config.macroFocusDistanceMilli) return false;
    bool lightKnown = false, lowLight = false;
    if (stats.luxMilli >= 0 && config.macroLuxMilli >= 0) {
        lightKnown = true;
        if (stats.luxMilli <= config.macroLuxMilli) lowLight = true;
    }
    if (config.macroMeanLumaMilli >= 0) {
        int32_t mean = stats.meanLumaMilli;
        bool have = mean >= 0;
        if (!have) have = histogramMeanLumaMilli(stats, mean);
        if (have) {
            lightKnown = true;
            if (mean <= config.macroMeanLumaMilli) lowLight = true;
        }
    }
    return lightKnown && lowLight;
}

int32_t QCamera3SceneBrightness::decide(const Stats &stats, const Config &config,
        uint32_t frameNumber, Result &result, Decision &decision,
        const std::atomic<bool> *cancelFlag) {
    if (!Config::valid(config) || !validStats(stats)) {
        return fail(result, frameNumber, -EINVAL, kReasonNoTarget);
    }
    if (stopped(cancelFlag)) {
        return fail(result, frameNumber, -ECANCELED, kReasonNoTarget);
    }
    if (!usableStats(stats)) {
        // No real statistic at all: nothing is decided and the bounded state is
        // left untouched. The caller may tick the inactivity timer with idle().
        return fail(result, frameNumber, -ENODATA, kReasonNoTarget);
    }

    uint32_t gap = 1;
    if (frameNumber <= mState.lastFrame) reset();
    else if (mState.lastFrame != 0) gap = frameNumber - mState.lastFrame;
    mState.lastFrame = frameNumber;

    const bool macroConfigured = config.macroFocusDistanceMilli >= 0;
    const bool detected = lowLightMacro(stats, config);
    // The re-entry lock counts real frame distance, not calls.
    const uint32_t maxGap = MAX_COOLDOWN_FRAMES;
    if (gap > maxGap) gap = maxGap;
    if (mState.cooldownRemaining > 0) {
        mState.cooldownRemaining -= int32_t(gap);
        if (mState.cooldownRemaining < 0) mState.cooldownRemaining = 0;
    }
    const bool cooldownActive = mState.cooldownRemaining > 0;

    int32_t macroReason = kReasonDisabled;
    if (!macroConfigured) {
        mState.enterCount = 0;
        if (mState.macro) {
            // The real thresholds disappeared (reconfigure): release at once.
            mState.macro = 0;
            mState.heldFrames = 0;
            mState.idleFrames = 0;
            mState.cooldownRemaining = config.cooldownFrames;
            macroReason = kReasonMacroReleased;
        }
    } else if (detected) {
        mState.idleFrames = 0;
        if (mState.macro) {
            ++mState.heldFrames;
            macroReason = kReasonMacro;
        } else if (cooldownActive) {
            mState.enterCount = 0;
            macroReason = kReasonCooldown;
        } else if (stats.motionMilli >= 0 && stats.motionMilli >= config.motionHoldMilli) {
            // Real motion blocks a new entry; it never releases a held state.
            mState.enterCount = 0;
            macroReason = kReasonMacroBlocked;
        } else {
            ++mState.enterCount;
            if (mState.enterCount >= config.enterFrames) {
                mState.enterCount = 0;
                mState.macro = 1;
                mState.heldFrames = 1;
                macroReason = kReasonMacro;
            } else {
                macroReason = kReasonMacroEntering;
            }
        }
    } else {
        mState.enterCount = 0;
        if (mState.macro) {
            // Absence hysteresis: the low-light macro state is released only
            // after inactivityFrames consecutive absence frames.
            ++mState.idleFrames;
            if (mState.idleFrames >= config.inactivityFrames) {
                mState.macro = 0;
                mState.heldFrames = 0;
                mState.idleFrames = 0;
                mState.cooldownRemaining = config.cooldownFrames;
                macroReason = kReasonMacroReleased;
            } else {
                macroReason = kReasonMacro;
            }
        } else {
            macroReason = cooldownActive ? kReasonCooldown : kReasonNoTarget;
        }
    }

    int32_t brightness = 0;
    int32_t sceneId = 0;
    const bool targeted = brightnessTarget(stats, config, brightness);
    if (targeted) sceneId = stats.sceneClass;

    Decision decided;
    decided.applied = targeted ? 1 : 0;
    decided.brightness = targeted ? brightness : 0;
    decided.sceneId = sceneId;
    decided.macro = mState.macro;
    decided.reason = targeted ? kReasonApplied : macroReason;
    decided.heldFrames = mState.heldFrames;
    decided.idleFrames = mState.idleFrames;
    decision = decided;
    mState.lastReason = decided.reason;
    mState.lastBrightness = decided.brightness;
    mState.lastScene = sceneId;

    Result ok;
    ok.status = 0;
    ok.applied = targeted ? 1 : 0;
    ok.frameNumber = frameNumber;
    ok.detail[0] = decided.reason;
    ok.detail[1] = mState.macro;
    ok.detail[2] = mState.idleFrames;
    ok.detail[3] = decided.brightness;
    result = ok;
    return 0;
}

int32_t QCamera3SceneBrightness::idle(uint32_t frameNumber, const Config &config,
        Result &result, Decision &decision, const std::atomic<bool> *cancelFlag) {
    if (!Config::valid(config)) {
        return fail(result, frameNumber, -EINVAL, kReasonNoTarget);
    }
    if (stopped(cancelFlag)) {
        return fail(result, frameNumber, -ECANCELED, kReasonNoTarget);
    }
    if (frameNumber <= mState.lastFrame) reset();
    uint32_t gap = mState.lastFrame == 0 ? 1 : frameNumber - mState.lastFrame;
    const uint32_t maxGap = MAX_INACTIVITY_FRAMES;
    if (gap > maxGap) gap = maxGap;
    mState.lastFrame = frameNumber;

    const uint32_t maxCooldownGap = MAX_COOLDOWN_FRAMES;
    if (gap > maxCooldownGap) gap = maxCooldownGap;
    if (mState.cooldownRemaining > 0) {
        mState.cooldownRemaining -= int32_t(gap);
        if (mState.cooldownRemaining < 0) mState.cooldownRemaining = 0;
    }
    const bool cooldownActive = mState.cooldownRemaining > 0;

    int32_t reason = kReasonIdle;
    if (mState.macro) {
        mState.idleFrames += int32_t(gap);
        if (mState.idleFrames >= config.inactivityFrames) {
            mState.macro = 0;
            mState.heldFrames = 0;
            mState.idleFrames = 0;
            mState.cooldownRemaining = config.cooldownFrames;
            reason = kReasonMacroReleased;
        } else {
            reason = kReasonMacro;
        }
    } else if (config.macroFocusDistanceMilli < 0) {
        reason = kReasonDisabled;
    } else if (cooldownActive) {
        reason = kReasonCooldown;
    }

    Decision ticked;
    ticked.applied = 0;
    ticked.brightness = 0;
    ticked.sceneId = 0;
    ticked.macro = mState.macro;
    ticked.reason = reason;
    ticked.heldFrames = mState.heldFrames;
    ticked.idleFrames = mState.idleFrames;
    decision = ticked;
    mState.lastReason = reason;

    Result ok;
    ok.status = 0;
    ok.frameNumber = frameNumber;
    ok.detail[0] = reason;
    ok.detail[1] = mState.macro;
    ok.detail[2] = mState.idleFrames;
    result = ok;
    return 0;
}

void QCamera3SceneBrightness::reset() {
    mState = State();
}

} // namespace qcamera
