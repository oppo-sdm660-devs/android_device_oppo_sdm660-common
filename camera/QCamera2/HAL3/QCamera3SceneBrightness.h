/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_SCENE_BRIGHTNESS_H
#define QCAMERA3_SCENE_BRIGHTNESS_H

#include <atomic>
#include <stddef.h>
#include <stdint.h>

namespace qcamera {

// Scene-conditional brightness target and low-light macro detection over real
// frame statistics, with an inactivity timeout and re-entry rules.
//
// Stock behavior behind this policy: the R11/R11s HAL logged "Failed to
// setBrightness for snow scene", "HALDBG: set SNOW brightness %d", kept
// "inactivateTimer timerLowLitMacroScene" and switched its lens state on
// "low light / Macro scene" with a timer for a "well lit, non-macro scene"
// (states tele/transition/wide). Evidence: stock-analysis items
// `hal.scene_brightness` and `plugin.asd.mask`. The stock per-scene brightness
// values, the ASD scene-id mapping and the stock timer length were not
// recovered, so this stage does not reuse them.
//
// The stage is a pure decision function over real statistics: it never creates
// a statistic, never guesses a missing one, and never applies a scene target
// the caller did not decode. A real scene-id target table that is empty means
// "no scene-conditional brightness", which is reported, not invented.
//
// Explicit non-claims: this is a preview/still brightness *target* in the real
// decoded QCameraParameters brightness range, not a display backlight control
// and not an AE/AWB algorithm; the low-light macro predicate is a documented
// threshold rule over real focus distance and real light statistics, not the
// stock FOV-control state machine and not a lens-position measurement. See
// docs/SCENE_BRIGHTNESS.md.
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
        kReasonApplied = 0,        // a real scene target changed the brightness
        kReasonNoTarget = 1,       // no decoded target for this scene
        kReasonMacroEntering = 2,  // low-light macro detected, entry hysteresis running
        kReasonMacro = 3,          // low-light macro state held
        kReasonMacroReleased = 4,  // released after the inactivity timeout
        kReasonMacroBlocked = 5,   // real motion blocks a new entry
        kReasonCooldown = 6,       // re-entry lock after a release
        kReasonIdle = 7,           // timer tick without real statistics
        kReasonDisabled = 8,       // no decoded macro threshold (detection off)
    };

    // Real per-frame statistics. Absent fields are negative; the policy never
    // substitutes a value for them.
    struct Stats {
        const uint32_t *lumaHistogram = nullptr;  // 256 bins, bin i = luma i
        uint32_t histogramTotal = 0;              // must equal the bin sum
        int32_t meanLumaMilli = -1;               // real mean luma * 1000 (0..255000)
        int32_t focusDistanceMilli = -1;          // real diopters * 1000
        int32_t sceneClass = -1;                  // real classified scene id
        int32_t luxMilli = -1;                    // real lux * 1000
        int32_t motionMilli = -1;                 // real 0..1000 motion estimate
    };

    // One real decoded per-scene brightness target. `sceneId` must be the id the
    // real classifier emits; this stage does not map class names to ids.
    struct SceneTarget {
        int32_t sceneId = 0;
        int32_t brightness = 0;
    };

    // Request-side struct. House policy stages name it Config; Options is the
    // frozen-contract alias, so Options::valid() is available as well.
    struct Config {
        // Real decoded preview brightness range and step (QCameraParameters
        // brightness min/max/step). No target outside this range is admitted.
        int32_t brightnessMin = 0;
        int32_t brightnessMax = 0;
        int32_t brightnessStep = 1;
        const SceneTarget *targets = nullptr;
        uint32_t targetCount = 0;
        // Real decoded low-light macro thresholds. -1 = not decoded, and the
        // corresponding condition is then never invented.
        int32_t macroFocusDistanceMilli = -1; // macro at or above this distance
        int32_t macroLuxMilli = -1;           // low light at or below this lux
        int32_t macroMeanLumaMilli = -1;      // low light at or below this luma
        int32_t enterFrames = 3;              // consecutive detections to enter
        int32_t inactivityFrames = 30;        // absence frames to release
        int32_t cooldownFrames = 20;          // re-entry lock after a release
        int32_t motionHoldMilli = 800;        // real motion at/above blocks entry
        static bool valid(const Config &c);
    };
    using Options = Config;   // frozen-contract name for the request-side struct

    struct Decision {
        int32_t applied = 0;      // 1 = brightness below is a real applied target
        int32_t brightness = 0;   // target brightness when applied
        int32_t sceneId = 0;      // scene the target came from (0 = none)
        int32_t macro = 0;        // 1 = low-light macro state is held
        int32_t reason = kReasonNoTarget;
        int32_t heldFrames = 0;   // frames the macro state has been held
        int32_t idleFrames = 0;   // consecutive absence frames
    };

    struct Result {
        int32_t status = 0;    // 0 pass; negative errno otherwise
        int32_t applied = 0;   // 1 only when a brightness target was applied
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

    static const char *name();  // trace id "SceneBrightness"
    // Pure policy: available whenever the caller can supply real statistics.
    static bool isAvailable();
    static bool supports(uint32_t width, uint32_t height);
    static bool validConfig(const Config &config);
    static bool validOptions(const Config &config) { return Config::valid(config); }
    // Malformed present statistics return false; absent fields are allowed.
    static bool validStats(const Stats &stats);
    // True only when some real statistic can drive the policy.
    static bool usableStats(const Stats &stats);
    // Real histogram mean luma in milli-levels. False without a consistent
    // histogram (null, zero total, or bin sum != histogramTotal).
    static bool histogramMeanLumaMilli(const Stats &stats, int32_t &meanMilli);
    // Documented low-light macro predicate over real inputs. Returns true only
    // when both a real light condition and a real macro focus condition hold.
    static bool lowLightMacro(const Stats &stats, const Config &config);

    // One call per decided frame, monotonic frame numbers. Real statistics
    // advance the macro state and may apply a scene brightness target.
    int32_t decide(const Stats &stats, const Config &config, uint32_t frameNumber,
            Result &result, Decision &decision,
            const std::atomic<bool> *cancelled = nullptr);

    // Timer tick without real statistics: advances the inactivity timer and may
    // release the macro state. Never applies a brightness target.
    int32_t idle(uint32_t frameNumber, const Config &config, Result &result,
            Decision &decision, const std::atomic<bool> *cancelled = nullptr);

    void reset();
    const State &state() const { return mState; }

private:
    State mState;
};

} // namespace qcamera
#endif
