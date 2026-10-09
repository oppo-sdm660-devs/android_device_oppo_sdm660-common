/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_FLASH_SEQUENCE_H
#define QCAMERA3_FLASH_SEQUENCE_H

#include <atomic>
#include <stddef.h>
#include <stdint.h>

namespace qcamera {

// Multi-frame flash sequencing and dual-tone LED calibration for a still
// capture. This stage plans and validates; it never submits a request.
//
// Stock behavior behind this plan: the R11/R11s HAL logged "flash multiframe
// flash_muti_num:%d", "flash multiframe numSnapshots = %d, numRetroSnapshots =
// %d", applied a "Dual LED calibration value = %d" and exposed
// `persist.camera.led.on.period`, `persist.camera.retro.number` and
// `persist.camera.dualled_calib`. Evidence: stock-analysis items
// `hal.flash.multiframe` and `hal.flash.dual_led_calib`. That code is HAL1
// parameter records plus a stock tuning table that was not recovered, so this
// stage does not reuse it.
//
// Every number is real caller input: the decoded flash capability, the real AE
// state, the real LED-on bound and a real per-unit dual-LED calibration value
// with its decoded scale. A missing value refuses (-ENODATA) and is never
// defaulted, interpolated from another field or replaced with "neutral".
//
// Explicit non-claims: the stage does not know the stock `flash_muti_num`
// default, does not know the exact encoding of the stock dual-LED calibration
// value (the caller must supply the real value, its decoded range and the real
// tuning span), does not fire an LED, does not verify charge time or thermal
// limits, and cannot prove that the planned frames really merge. See
// docs/FLASH_SEQUENCE.md.
class QCamera3FlashSequence {
public:
    enum {
        MAX_STEPS = 4,               // pre-flash + main + at most 2 ambient
        MAX_AMBIENT_STEPS = 2,
        MAX_STEP_DURATION_NS = 2000000000LL,  // 2 s per planned step
        MAX_SEQUENCE_NS = 5000000000LL,       // 5 s for the whole sequence
        GAIN_Q10_UNITY = 1024,
        MAX_GAIN_Q10 = 2047,
        MAX_CALIBRATION_SPAN_Q10 = 1024,
    };

    // ANDROID_FLASH_MODE values (camera_metadata_tags.h).
    enum FlashMode {
        FLASH_MODE_OFF = 0,
        FLASH_MODE_SINGLE = 1,
        FLASH_MODE_TORCH = 2,
    };

    // ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER values.
    enum PrecaptureTrigger {
        PRECAPTURE_IDLE = 0,
        PRECAPTURE_START = 1,
        PRECAPTURE_CANCEL = 2,
    };

    enum Role {
        kRolePreflash = 1,
        kRoleMain = 2,
        kRoleAmbient = 3,
    };

    enum Reason {
        kReasonPlanned = 0,
        kReasonBypass = 1,             // flash OFF: one non-flash frame
        kReasonMalformed = 2,
        kReasonFlashUnknown = 3,       // flash availability never decoded
        kReasonNoFlashHardware = 4,    // decoded as unavailable
        kReasonTorchNotStill = 5,      // torch is not a still flash mode
        kReasonLedPeriod = 6,          // missing / zero / above the real bound
        kReasonStepBudget = 7,         // more steps than the bounded plan
        kReasonSequenceTooLong = 8,    // total planned duration above the bound
        kReasonPrecaptureUnsupported = 9,
        kReasonAeMissing = 10,         // real AE state absent
        kReasonAeNotConverged = 11,    // documented AE-convergence refusal
        kReasonAeRangeUnknown = 12,    // decoded exposure/ISO range absent
        kReasonExposureOutOfRange = 13,
        kReasonDualLedUnsupported = 14,
        kReasonCalibrationMissing = 15,
        kReasonCalibrationRange = 16,
        kReasonPlanMismatch = 17,      // caller plan != the real inputs
    };

    // Real decoded flash capability. `flashAvailable`, `precaptureAvailable`
    // and the range fields use -1 for "not decoded".
    struct Capability {
        int32_t flashAvailable = -1;       // ANDROID_FLASH_INFO_AVAILABLE 0/1
        int32_t dualLed = 0;               // 1 only when the unit really has it
        int64_t maxLedOnPeriodNs = -1;     // real decoded LED-on bound
        int32_t maxSteps = -1;             // real decoded flash_muti_num bound
        int32_t precaptureAvailable = -1;  // 1 when the trigger is advertised
        int64_t exposureMinNs = -1;
        int64_t exposureMaxNs = -1;
        int32_t isoMin = -1;
        int32_t isoMax = -1;
        bool hasAeRange() const {
            return exposureMinNs > 0 && exposureMaxNs >= exposureMinNs &&
                    isoMin > 0 && isoMax >= isoMin;
        }
    };

    // Real per-frame AE state of the frame the plan is built for.
    struct AeState {
        int32_t aeState = -1;            // ANDROID_CONTROL_AE_STATE (0..5)
        int32_t precaptureTrigger = -1;  // ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER
        int64_t ambientExposureNs = -1;  // real converged ambient exposure
        int32_t ambientIso = -1;         // real converged ambient sensitivity
        int32_t flashRequired = -1;      // real AE decision (FLASH_REQUIRED = 4)
    };

    // Real per-unit dual-tone LED calibration. `spanQ10` is the real tuning
    // span recovered from the unit's tuning data; 0 means it was not recovered
    // and the correction is refused instead of being replaced by unity.
    struct DualLedCalibration {
        int32_t present = 0;   // 1 only when a real value was read
        int32_t rawValue = 0;
        int32_t rawMin = 0;
        int32_t rawMax = 0;
        int32_t spanQ10 = 0;
    };

    struct Options {
        int32_t flashMode = FLASH_MODE_OFF;  // standard ANDROID_FLASH_MODE
        int32_t preflash = 0;                // 0/1
        int32_t ambientFrames = 0;           // 0..MAX_AMBIENT_STEPS
        int32_t dualLedCorrection = 0;       // 0/1
        int64_t ledOnPeriodNs = 0;           // real requested LED-on period
        int32_t overrideAe = 0;              // 0/1 apply the values below
        int64_t mainExposureNs = 0;          // real, > 0 when overrideAe
        int32_t mainIso = 0;                 // real, > 0 when overrideAe
        uint32_t frameNumber = 0;
        static bool valid(const Options &o);
    };

    struct Step {
        int32_t role = 0;               // Role
        int32_t flashOn = 0;            // 1 = LED on for this step
        int32_t precaptureTrigger = PRECAPTURE_IDLE;
        int64_t exposureNs = 0;         // 0 = keep the caller's ongoing AE value
        int32_t iso = 0;                // 0 = keep the caller's ongoing AE value
        int64_t ledOnPeriodNs = 0;      // 0 for flash-off steps
        int64_t durationNs = 0;         // bounded step duration
    };

    struct Correction {
        int32_t warmGainQ10 = GAIN_Q10_UNITY;  // applied to the warm LED
        int32_t coolGainQ10 = GAIN_Q10_UNITY;  // applied to the cool LED
        int32_t deltaQ10 = 0;                  // signed offset from unity
        int32_t calibrationValue = 0;
    };

    struct Plan {
        uint32_t steps = 0;
        Step step[MAX_STEPS];
        int64_t totalDurationNs = 0;
        uint32_t preflashSteps = 0;
        uint32_t ambientSteps = 0;
        int32_t overrideApplied = 0;
        int32_t dualLedApplied = 0;
        Correction correction;
    };

    struct Result {
        int32_t status = 0;    // 0 pass; negative errno otherwise
        int32_t applied = 0;   // 1 when the plan really fires the LED, else 0
        uint32_t frameNumber = 0;
        // detail[0] = Reason, detail[1] = planned step count,
        // detail[2] = total duration in ms, detail[3] = 1 when a dual-LED
        // correction is part of the plan.
        int32_t detail[4] = {};
    };

    static const char *name();  // trace id "FlashSequence"
    // Pure planner; the real flash presence comes from Capability input.
    static bool isAvailable();
    static bool supports(uint32_t width, uint32_t height);
    static bool validOptions(const Options &options);
    static bool validCapability(const Capability &capability);
    static bool validAeState(const AeState &ae);
    static bool validCalibration(const DualLedCalibration &calibration);

    // Builds the bounded sequence (pre-flash, main, optional ambient frames)
    // for the real AE state. The Plan is written only on success. A flash-OFF
    // request is the documented single-frame bypass (applied == 0).
    static int32_t plan(const Capability &capability, const AeState &ae,
            const DualLedCalibration *calibration, const Options &options,
            Result &result, Plan &plan, const std::atomic<bool> *cancelled = nullptr);

    // Re-derives the plan from the same real inputs and refuses any difference
    // (step count, roles, flash-on flags, AE overrides, LED periods, timing).
    // The caller's totalDurationNs is never trusted.
    static int32_t validate(const Capability &capability, const AeState &ae,
            const DualLedCalibration *calibration, const Options &options,
            const Plan &plan, Result &result,
            const std::atomic<bool> *cancelled = nullptr);

    // Validates the real per-unit calibration value and computes the applied
    // colour/gain correction. Refuses an absent value, an unrecovered span or a
    // value outside its decoded range; never substitutes a neutral correction.
    static int32_t dualLedCorrection(const Capability &capability,
            const DualLedCalibration &calibration, uint32_t frameNumber,
            Result &result, Correction &correction,
            const std::atomic<bool> *cancelled = nullptr);

    // No retained state exists; provided for the common stage shape.
    static void reset();
};

} // namespace qcamera
#endif
