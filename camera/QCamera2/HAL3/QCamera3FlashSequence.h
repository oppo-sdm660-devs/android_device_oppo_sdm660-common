/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_FLASH_SEQUENCE_H
#define QCAMERA3_FLASH_SEQUENCE_H

#include <atomic>
#include <stddef.h>
#include <stdint.h>

namespace qcamera {

// Multi-frame flash sequencing and dual-tone LED calibration; plans only, never submits.
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
        kReasonBypass = 1,
        kReasonMalformed = 2,
        kReasonFlashUnknown = 3,
        kReasonNoFlashHardware = 4,
        kReasonTorchNotStill = 5,
        kReasonLedPeriod = 6,
        kReasonStepBudget = 7,
        kReasonSequenceTooLong = 8,
        kReasonPrecaptureUnsupported = 9,
        kReasonAeMissing = 10,
        kReasonAeNotConverged = 11,
        kReasonAeRangeUnknown = 12,
        kReasonExposureOutOfRange = 13,
        kReasonDualLedUnsupported = 14,
        kReasonCalibrationMissing = 15,
        kReasonCalibrationRange = 16,
        kReasonPlanMismatch = 17,
    };

    // -1 = not decoded (flashAvailable, precaptureAvailable, ranges).
    struct Capability {
        int32_t flashAvailable = -1;       // ANDROID_FLASH_INFO_AVAILABLE 0/1
        int32_t dualLed = 0;
        int64_t maxLedOnPeriodNs = -1;
        int32_t maxSteps = -1; // decoded flash_muti_num bound
        int32_t precaptureAvailable = -1;
        int64_t exposureMinNs = -1;
        int64_t exposureMaxNs = -1;
        int32_t isoMin = -1;
        int32_t isoMax = -1;
        bool hasAeRange() const {
            return exposureMinNs > 0 && exposureMaxNs >= exposureMinNs &&
                    isoMin > 0 && isoMax >= isoMin;
        }
    };

    // Per-frame AE state the plan is built for.
    struct AeState {
        int32_t aeState = -1;            // ANDROID_CONTROL_AE_STATE (0..5)
        int32_t precaptureTrigger = -1;  // ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER
        int64_t ambientExposureNs = -1;
        int32_t ambientIso = -1;
        int32_t flashRequired = -1; // AE decision (FLASH_REQUIRED = 4)
    };

    // Per-unit dual-tone LED calibration; spanQ10 = 0 means the tuning span was
    // not recovered and the correction is refused.
    struct DualLedCalibration {
        int32_t present = 0;
        int32_t rawValue = 0;
        int32_t rawMin = 0;
        int32_t rawMax = 0;
        int32_t spanQ10 = 0;
    };

    struct Options {
        int32_t flashMode = FLASH_MODE_OFF;  // standard ANDROID_FLASH_MODE
        int32_t preflash = 0;
        int32_t ambientFrames = 0;
        int32_t dualLedCorrection = 0;
        int64_t ledOnPeriodNs = 0;
        int32_t overrideAe = 0;     // 0/1: apply the values below
        int64_t mainExposureNs = 0; // > 0 when overrideAe
        int32_t mainIso = 0; // > 0 when overrideAe
        uint32_t frameNumber = 0;
        static bool valid(const Options &o);
    };

    struct Step {
        int32_t role = 0;
        int32_t flashOn = 0;
        int32_t precaptureTrigger = PRECAPTURE_IDLE;
        int64_t exposureNs = 0;         // 0 = keep the caller's ongoing AE value
        int32_t iso = 0;                // 0 = keep the caller's ongoing AE value
        int64_t ledOnPeriodNs = 0;      // 0 for flash-off steps
        int64_t durationNs = 0;
    };

    struct Correction {
        int32_t warmGainQ10 = GAIN_Q10_UNITY;
        int32_t coolGainQ10 = GAIN_Q10_UNITY;
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
        int32_t applied = 0;
        uint32_t frameNumber = 0;
        // detail[0] = Reason, detail[1] = planned step count,
        // detail[2] = total duration in ms, detail[3] = 1 when a dual-LED
        // correction is part of the plan.
        int32_t detail[4] = {};
    };

    static const char *name();
    static bool isAvailable();
    static bool supports(uint32_t width, uint32_t height);
    static bool validOptions(const Options &options);
    static bool validCapability(const Capability &capability);
    static bool validAeState(const AeState &ae);
    static bool validCalibration(const DualLedCalibration &calibration);

    // Builds pre-flash/main/ambient steps for the real AE state; the Plan is
    // written only on success.
    static int32_t plan(const Capability &capability, const AeState &ae,
            const DualLedCalibration *calibration, const Options &options,
            Result &result, Plan &plan, const std::atomic<bool> *cancelled = nullptr);

    // Re-derives the plan and refuses any difference; the caller's totalDurationNs
    // is not trusted.
    static int32_t validate(const Capability &capability, const AeState &ae,
            const DualLedCalibration *calibration, const Options &options,
            const Plan &plan, Result &result,
            const std::atomic<bool> *cancelled = nullptr);

    // Computes the colour/gain correction from the real calibration value; refuses
    // an absent value, an unrecovered span or a value outside its decoded range.
    static int32_t dualLedCorrection(const Capability &capability,
            const DualLedCalibration &calibration, uint32_t frameNumber,
            Result &result, Correction &correction,
            const std::atomic<bool> *cancelled = nullptr);

    static void reset();
};

} // namespace qcamera
#endif
