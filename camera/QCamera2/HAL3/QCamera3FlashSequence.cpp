/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3FlashSequence.h"

#include <errno.h>

namespace qcamera {
namespace {

using Flash = QCamera3FlashSequence;

constexpr int32_t kMaxIso = 1024000;              // same bound as the AE policy
constexpr int64_t kMaxRealExposureNs = 60000000000LL; // 60 s sanity bound
constexpr int32_t kMaxDecodedSteps = 64;

// ANDROID_CONTROL_AE_STATE values.
constexpr int32_t kAeStateConverged = 2;
constexpr int32_t kAeStateLocked = 3;

bool stopped(const std::atomic<bool> *flag) { return flag && flag->load(); }

int32_t fail(Flash::Result &result, uint32_t frameNumber, int32_t status, int32_t reason,
        int32_t detail1 = -1, int32_t detail2 = 0, int32_t detail3 = 0) {
    Flash::Result local;
    local.frameNumber = frameNumber;
    local.status = status;
    local.detail[0] = reason;
    local.detail[1] = detail1;
    local.detail[2] = detail2;
    local.detail[3] = detail3;
    result = local;
    return status;
}

int64_t stepDuration(const Flash::Step &step, int64_t ambientExposureNs) {
    int64_t duration = step.ledOnPeriodNs;
    if (step.exposureNs > duration) duration = step.exposureNs;
    if (duration == 0) duration = ambientExposureNs;
    return duration;
}

bool planEqual(const Flash::Plan &a, const Flash::Plan &b) {
    if (a.steps != b.steps || a.totalDurationNs != b.totalDurationNs ||
            a.preflashSteps != b.preflashSteps || a.ambientSteps != b.ambientSteps ||
            a.overrideApplied != b.overrideApplied ||
            a.dualLedApplied != b.dualLedApplied) return false;
    if (a.correction.warmGainQ10 != b.correction.warmGainQ10 ||
            a.correction.coolGainQ10 != b.correction.coolGainQ10 ||
            a.correction.deltaQ10 != b.correction.deltaQ10 ||
            a.correction.calibrationValue != b.correction.calibrationValue) return false;
    for (uint32_t i = 0; i < a.steps && i < Flash::MAX_STEPS; ++i) {
        const Flash::Step &x = a.step[i];
        const Flash::Step &y = b.step[i];
        if (x.role != y.role || x.flashOn != y.flashOn ||
                x.precaptureTrigger != y.precaptureTrigger ||
                x.exposureNs != y.exposureNs || x.iso != y.iso ||
                x.ledOnPeriodNs != y.ledOnPeriodNs ||
                x.durationNs != y.durationNs) return false;
    }
    return true;
}

} // namespace

bool QCamera3FlashSequence::isAvailable() { return true; }

const char *QCamera3FlashSequence::name() { return "FlashSequence"; }

bool QCamera3FlashSequence::supports(uint32_t width, uint32_t height) {
    return width >= 16 && height >= 16 && width <= 16384 && height <= 16384 &&
            !(width & 1) && !(height & 1);
}

bool QCamera3FlashSequence::Options::valid(const Options &o) {
    if (o.flashMode != FLASH_MODE_OFF && o.flashMode != FLASH_MODE_SINGLE &&
            o.flashMode != FLASH_MODE_TORCH) return false;
    if (o.preflash != 0 && o.preflash != 1) return false;
    if (o.ambientFrames < 0 || o.ambientFrames > MAX_AMBIENT_STEPS) return false;
    if (o.dualLedCorrection != 0 && o.dualLedCorrection != 1) return false;
    if (o.ledOnPeriodNs < 0 || o.ledOnPeriodNs > MAX_STEP_DURATION_NS) return false;
    if (o.overrideAe != 0 && o.overrideAe != 1) return false;
    if (o.mainExposureNs < 0 || o.mainExposureNs > MAX_STEP_DURATION_NS) return false;
    if (o.mainIso < 0 || o.mainIso > kMaxIso) return false;
    if (o.overrideAe == 1 && (o.mainExposureNs <= 0 || o.mainIso <= 0)) return false;
    // A flash-off request has no pre-flash frame and no dual-LED correction.
    if (o.flashMode == FLASH_MODE_OFF &&
            (o.preflash != 0 || o.dualLedCorrection != 0)) return false;
    return true;
}

bool QCamera3FlashSequence::validCapability(const Capability &c) {
    if (c.flashAvailable < -1 || c.flashAvailable > 1) return false;
    if (c.dualLed != 0 && c.dualLed != 1) return false;
    if (c.maxLedOnPeriodNs < -1 || c.maxLedOnPeriodNs > kMaxRealExposureNs) return false;
    if (c.maxSteps < -1 || c.maxSteps > kMaxDecodedSteps) return false;
    if (c.precaptureAvailable < -1 || c.precaptureAvailable > 1) return false;
    const bool anyRange = c.exposureMinNs >= 0 || c.exposureMaxNs >= 0 ||
            c.isoMin >= 0 || c.isoMax >= 0;
    if (!anyRange) return true;   // the whole range is undecoded
    if (c.exposureMinNs <= 0 || c.exposureMaxNs < c.exposureMinNs ||
            c.exposureMaxNs > kMaxRealExposureNs) return false;
    if (c.isoMin <= 0 || c.isoMax < c.isoMin || c.isoMax > kMaxIso) return false;
    return true;
}

bool QCamera3FlashSequence::validAeState(const AeState &ae) {
    if (ae.aeState < -1 || ae.aeState > 5) return false;
    if (ae.precaptureTrigger < -1 || ae.precaptureTrigger > PRECAPTURE_CANCEL) return false;
    if (ae.ambientExposureNs < -1 || ae.ambientExposureNs > kMaxRealExposureNs) return false;
    if (ae.ambientIso < -1 || ae.ambientIso > kMaxIso) return false;
    if (ae.flashRequired < -1 || ae.flashRequired > 1) return false;
    return true;
}

bool QCamera3FlashSequence::validCalibration(const DualLedCalibration &c) {
    if (c.present != 0 && c.present != 1) return false;
    if (c.spanQ10 < 0 || c.spanQ10 > MAX_CALIBRATION_SPAN_Q10) return false;
    return c.rawMax >= c.rawMin;
}

bool QCamera3FlashSequence::validOptions(const Options &options) {
    return Options::valid(options);
}

int32_t QCamera3FlashSequence::dualLedCorrection(const Capability &capability,
        const DualLedCalibration &calibration, uint32_t frameNumber, Result &result,
        Correction &correction, const std::atomic<bool> *cancelFlag) {
    if (!validCapability(capability) || !validCalibration(calibration)) {
        return fail(result, frameNumber, -EINVAL, kReasonMalformed);
    }
    if (stopped(cancelFlag)) {
        return fail(result, frameNumber, -ECANCELED, kReasonMalformed);
    }
    if (capability.dualLed != 1) {
        return fail(result, frameNumber, -EPROTONOSUPPORT, kReasonDualLedUnsupported);
    }
    if (calibration.present != 1) {
        // Never invent a value: without a real per-unit read there is no
        // correction, and the caller must not apply a neutral one either.
        return fail(result, frameNumber, -ENODATA, kReasonCalibrationMissing);
    }
    if (calibration.spanQ10 <= 0) {
        // A real value with an unrecovered tuning span cannot produce a gain.
        return fail(result, frameNumber, -ENODATA, kReasonCalibrationMissing,
                calibration.rawValue);
    }
    if (calibration.rawMax <= calibration.rawMin) {
        return fail(result, frameNumber, -EINVAL, kReasonCalibrationRange,
                calibration.rawValue);
    }
    if (calibration.rawValue < calibration.rawMin ||
            calibration.rawValue > calibration.rawMax) {
        return fail(result, frameNumber, -ERANGE, kReasonCalibrationRange,
                calibration.rawValue);
    }

    // Documented integer interpolation: the calibration value's position inside
    // its real decoded range maps linearly onto [-span, +span] Q10, and the warm
    // and cool LED contributions receive opposite offsets from unity gain.
    const int64_t range = int64_t(calibration.rawMax) - int64_t(calibration.rawMin);
    const int64_t numerator = int64_t(calibration.spanQ10) *
            (2 * int64_t(calibration.rawValue) - int64_t(calibration.rawMin) -
             int64_t(calibration.rawMax));
    int64_t delta = numerator / range;
    if (delta > calibration.spanQ10) delta = calibration.spanQ10;
    if (delta < -int64_t(calibration.spanQ10)) delta = -int64_t(calibration.spanQ10);
    int64_t warm = int64_t(GAIN_Q10_UNITY) + delta;
    int64_t cool = int64_t(GAIN_Q10_UNITY) - delta;
    if (warm < 1) warm = 1;
    if (warm > MAX_GAIN_Q10) warm = MAX_GAIN_Q10;
    if (cool < 1) cool = 1;
    if (cool > MAX_GAIN_Q10) cool = MAX_GAIN_Q10;

    Correction local;
    local.warmGainQ10 = int32_t(warm);
    local.coolGainQ10 = int32_t(cool);
    local.deltaQ10 = int32_t(delta);
    local.calibrationValue = calibration.rawValue;
    correction = local;

    Result applied;
    applied.status = 0;
    applied.applied = 1;
    applied.frameNumber = frameNumber;
    applied.detail[0] = kReasonPlanned;
    applied.detail[1] = local.deltaQ10;
    applied.detail[2] = calibration.rawValue;
    applied.detail[3] = calibration.spanQ10;
    result = applied;
    return 0;
}

int32_t QCamera3FlashSequence::plan(const Capability &capability, const AeState &ae,
        const DualLedCalibration *calibration, const Options &options, Result &result,
        Plan &plan, const std::atomic<bool> *cancelFlag) {
    const uint32_t frameNumber = options.frameNumber;
    if (!validOptions(options)) {
        return fail(result, frameNumber, -EINVAL, kReasonMalformed);
    }
    if (!validCapability(capability) || !validAeState(ae)) {
        return fail(result, frameNumber, -EINVAL, kReasonMalformed);
    }
    if (options.flashMode == FLASH_MODE_TORCH) {
        // A torch is a continuous light, not a still-capture flash sequence.
        return fail(result, frameNumber, -EINVAL, kReasonTorchNotStill);
    }
    if (stopped(cancelFlag)) {
        return fail(result, frameNumber, -ECANCELED, kReasonMalformed);
    }

    const bool flashOn = options.flashMode == FLASH_MODE_SINGLE;
    const uint32_t steps = 1 + (options.preflash ? 1u : 0u) +
            uint32_t(options.ambientFrames);
    uint32_t maxSteps = MAX_STEPS;
    if (capability.maxSteps >= 1 && uint32_t(capability.maxSteps) < maxSteps) {
        maxSteps = uint32_t(capability.maxSteps);
    }
    if (steps > maxSteps) {
        return fail(result, frameNumber, -E2BIG, kReasonStepBudget, int32_t(steps),
                int32_t(maxSteps));
    }

    if (flashOn) {
        if (capability.flashAvailable < 0) {
            return fail(result, frameNumber, -ENODATA, kReasonFlashUnknown);
        }
        if (capability.flashAvailable == 0) {
            return fail(result, frameNumber, -ENODEV, kReasonNoFlashHardware);
        }
        if (capability.maxLedOnPeriodNs < 0) {
            return fail(result, frameNumber, -ENODATA, kReasonLedPeriod);
        }
        if (capability.maxLedOnPeriodNs == 0) {
            return fail(result, frameNumber, -ENODEV, kReasonLedPeriod);
        }
        if (options.ledOnPeriodNs < 1) {
            return fail(result, frameNumber, -EINVAL, kReasonLedPeriod);
        }
        if (options.ledOnPeriodNs > capability.maxLedOnPeriodNs) {
            return fail(result, frameNumber, -E2BIG, kReasonLedPeriod,
                    int32_t(options.ledOnPeriodNs / 1000000),
                    int32_t(capability.maxLedOnPeriodNs / 1000000));
        }
        if (options.preflash != 0) {
            if (capability.precaptureAvailable < 0) {
                return fail(result, frameNumber, -ENODATA, kReasonPrecaptureUnsupported);
            }
            if (capability.precaptureAvailable == 0) {
                return fail(result, frameNumber, -EPROTONOSUPPORT,
                        kReasonPrecaptureUnsupported);
            }
        }
    }

    if (options.overrideAe != 0) {
        if (ae.aeState < 0 || ae.ambientExposureNs <= 0 || ae.ambientIso <= 0) {
            return fail(result, frameNumber, -ENODATA, kReasonAeMissing,
                    ae.aeState, int32_t(ae.ambientExposureNs / 1000000),
                    ae.ambientIso);
        }
        if (ae.aeState != kAeStateConverged && ae.aeState != kAeStateLocked) {
            // Documented refusal: a bounded main exposure is only planned from a
            // converged (or locked) real AE state.
            return fail(result, frameNumber, -EAGAIN, kReasonAeNotConverged, ae.aeState);
        }
        if (!capability.hasAeRange()) {
            return fail(result, frameNumber, -ENODATA, kReasonAeRangeUnknown);
        }
        if (options.mainExposureNs < capability.exposureMinNs ||
                options.mainExposureNs > capability.exposureMaxNs ||
                options.mainIso < capability.isoMin ||
                options.mainIso > capability.isoMax) {
            return fail(result, frameNumber, -ERANGE, kReasonExposureOutOfRange,
                    int32_t(options.mainExposureNs / 1000000), options.mainIso);
        }
    }

    Correction correction;
    int32_t dualLedApplied = 0;
    if (options.dualLedCorrection != 0) {
        if (capability.dualLed != 1) {
            return fail(result, frameNumber, -EPROTONOSUPPORT, kReasonDualLedUnsupported);
        }
        if (!calibration) {
            return fail(result, frameNumber, -ENODATA, kReasonCalibrationMissing);
        }
        Result correctionResult;
        const int32_t rc = dualLedCorrection(capability, *calibration, frameNumber,
                correctionResult, correction, cancelFlag);
        if (rc != 0) {
            result = correctionResult;
            return rc;
        }
        dualLedApplied = 1;
    }

    const int64_t mainExposure = options.overrideAe != 0 ? options.mainExposureNs : 0;
    const int32_t mainIso = options.overrideAe != 0 ? options.mainIso : 0;
    const int64_t ambientExposure = ae.ambientExposureNs > 0 ? ae.ambientExposureNs : 0;

    Plan built;
    uint32_t index = 0;
    if (options.preflash != 0) {
        Step &step = built.step[index++];
        step.role = kRolePreflash;
        step.flashOn = 1;   // pre-flash implies FLASH_MODE_SINGLE (validated above)
        step.precaptureTrigger = PRECAPTURE_START;
        step.exposureNs = mainExposure;
        step.iso = mainIso;
        step.ledOnPeriodNs = options.ledOnPeriodNs;
        step.durationNs = stepDuration(step, ambientExposure);
        built.preflashSteps = 1;
    }
    {
        Step &step = built.step[index++];
        step.role = kRoleMain;
        step.flashOn = flashOn ? 1 : 0;
        step.precaptureTrigger = PRECAPTURE_IDLE;
        step.exposureNs = mainExposure;
        step.iso = mainIso;
        step.ledOnPeriodNs = flashOn ? options.ledOnPeriodNs : 0;
        step.durationNs = stepDuration(step, ambientExposure);
    }
    for (uint32_t i = 0; i < uint32_t(options.ambientFrames); ++i) {
        Step &step = built.step[index++];
        step.role = kRoleAmbient;
        step.flashOn = 0;
        step.precaptureTrigger = PRECAPTURE_IDLE;
        step.exposureNs = mainExposure;
        step.iso = mainIso;
        step.ledOnPeriodNs = 0;
        step.durationNs = stepDuration(step, ambientExposure);
        ++built.ambientSteps;
    }

    int64_t total = 0;
    for (uint32_t i = 0; i < index; ++i) {
        if (stopped(cancelFlag)) {
            return fail(result, frameNumber, -ECANCELED, kReasonMalformed);
        }
        const int64_t duration = built.step[i].durationNs;
        if (duration > MAX_STEP_DURATION_NS) {
            return fail(result, frameNumber, -E2BIG, kReasonStepBudget, int32_t(i));
        }
        if (total > MAX_SEQUENCE_NS - duration) {
            return fail(result, frameNumber, -E2BIG, kReasonSequenceTooLong,
                    int32_t(total / 1000000), int32_t(duration / 1000000));
        }
        total += duration;
    }
    built.steps = index;
    built.totalDurationNs = total;
    built.overrideApplied = options.overrideAe != 0 ? 1 : 0;
    built.dualLedApplied = dualLedApplied;
    built.correction = correction;
    plan = built;

    Result planned;
    planned.status = 0;
    planned.applied = flashOn ? 1 : 0;
    planned.frameNumber = frameNumber;
    planned.detail[0] = flashOn ? kReasonPlanned : kReasonBypass;
    planned.detail[1] = int32_t(index);
    planned.detail[2] = int32_t(total / 1000000);
    planned.detail[3] = dualLedApplied;
    result = planned;
    return 0;
}

int32_t QCamera3FlashSequence::validate(const Capability &capability, const AeState &ae,
        const DualLedCalibration *calibration, const Options &options, const Plan &plan,
        Result &result, const std::atomic<bool> *cancelFlag) {
    Plan expected;
    Result expectedResult;
    const int32_t rc = QCamera3FlashSequence::plan(capability, ae, calibration, options,
            expectedResult, expected, cancelFlag);
    if (rc != 0) {
        result = expectedResult;
        return rc;
    }
    if (!planEqual(expected, plan)) {
        return fail(result, options.frameNumber, -EINVAL, kReasonPlanMismatch,
                int32_t(plan.steps), int32_t(expected.steps),
                plan.totalDurationNs == expected.totalDurationNs ? 0 : 1);
    }
    result = expectedResult;
    return 0;
}

void QCamera3FlashSequence::reset() {}

} // namespace qcamera
