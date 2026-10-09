/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_SCENE_BRIGHTNESS_H
#define QCAMERA3_SCENE_BRIGHTNESS_H

#include <atomic>
#include <stddef.h>
#include <stdint.h>

namespace qcamera {

// Scene-conditional brightness target and low-light macro detection over real
// frame statistics, with an inactivity timeout and re-entry rules.
class QCamera3SceneBrightness {
public:
    enum {
        kHistogramBins = 256,
        MAX_SCENE_TARGETS = 8,
        MAX_ENTER_FRAMES = 30,
        MAX_INACTIVITY_FRAMES = 300,
        MAX_COOLDOWN_FRAMES = 300,
    };

    enum Reason {
        kReasonApplied = 0,
        kReasonNoTarget = 1,
        kReasonMacroEntering = 2,
        kReasonMacro = 3,
        kReasonMacroReleased = 4,
        kReasonMacroBlocked = 5,
        kReasonCooldown = 6,
        kReasonIdle = 7,
        kReasonDisabled = 8,
    };

    // Per-frame statistics; -1 means the field is absent.
    struct Stats {
        const uint32_t *lumaHistogram = nullptr; // 256 bins, bin i = luma i
        uint32_t histogramTotal = 0;             // must equal the bin sum
        int32_t meanLumaMilli = -1;              // mean luma * 1000, 0..255000
        int32_t focusDistanceMilli = -1;         // diopters * 1000
        int32_t sceneClass = -1;
        int32_t luxMilli = -1;    // lux * 1000
        int32_t motionMilli = -1; // 0..1000
    };

    // `sceneId` must be the id the classifier emits; names are not mapped.
    struct SceneTarget {
        int32_t sceneId = 0;
        int32_t brightness = 0;
    };

    struct Config {
        // Decoded QCameraParameters brightness min/max/step.
        int32_t brightnessMin = 0;
        int32_t brightnessMax = 0;
        int32_t brightnessStep = 1;
        const SceneTarget *targets = nullptr;
        uint32_t targetCount = 0;
        // -1 = not decoded; that condition then never fires.
        int32_t macroFocusDistanceMilli = -1; // macro at or above this distance
        int32_t macroLuxMilli = -1;           // low light at or below this lux
        int32_t macroMeanLumaMilli = -1;      // low light at or below this luma
        int32_t enterFrames = 3;
        int32_t inactivityFrames = 30;
        int32_t cooldownFrames = 20;
        int32_t motionHoldMilli = 800; // motion at/above blocks entry
        static bool valid(const Config &c);
    };
    using Options = Config;

    struct Decision {
        int32_t applied = 0;
        int32_t brightness = 0;
        int32_t sceneId = 0; // 0 = none
        int32_t macro = 0;
        int32_t reason = kReasonNoTarget;
        int32_t heldFrames = 0;
        int32_t idleFrames = 0;
    };

    struct Result {
        int32_t status = 0;    // 0 pass; negative errno otherwise
        int32_t applied = 0;
        uint32_t frameNumber = 0;
        // detail[0] = Reason, detail[1] = macro state, detail[2] = idleFrames,
        // detail[3] = applied brightness (0 when none).
        int32_t detail[4] = {};
    };

    struct State {
        int32_t macro = 0;
        int32_t enterCount = 0;
        int32_t idleFrames = 0;
        int32_t cooldownRemaining = 0;
        int32_t heldFrames = 0;
        uint32_t lastFrame = 0;
        int32_t lastReason = kReasonNoTarget;
        int32_t lastBrightness = 0;
        int32_t lastScene = 0;
    };

    static const char *name();
    static bool isAvailable();
    static bool supports(uint32_t width, uint32_t height);
    static bool validConfig(const Config &config);
    static bool validOptions(const Config &config) { return Config::valid(config); }
    // Malformed present statistics return false; absent fields are allowed.
    static bool validStats(const Stats &stats);
    static bool usableStats(const Stats &stats);
    // False without a consistent histogram.
    static bool histogramMeanLumaMilli(const Stats &stats, int32_t &meanMilli);
    // True only when a light condition and a macro focus condition both hold.
    static bool lowLightMacro(const Stats &stats, const Config &config);

    // One call per decided frame, monotonic frame numbers.
    int32_t decide(const Stats &stats, const Config &config, uint32_t frameNumber,
            Result &result, Decision &decision,
            const std::atomic<bool> *cancelled = nullptr);

    // Advances the inactivity timer and may release the macro state.
    int32_t idle(uint32_t frameNumber, const Config &config, Result &result,
            Decision &decision, const std::atomic<bool> *cancelled = nullptr);

    void reset();
    const State &state() const { return mState; }

private:
    State mState;
};

} // namespace qcamera
#endif
