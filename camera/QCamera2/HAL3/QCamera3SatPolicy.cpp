/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3SatPolicy.h"

#include <errno.h>
#include <math.h>

namespace qcamera {
namespace {

constexpr float kPi = 3.14159265358979323846f;

int32_t milli(float value) {
    return int32_t(value * 1000.0f + (value >= 0.0f ? 0.5f : -0.5f));
}

bool validThresholds(const QCamera3SatPolicy::Thresholds &thresholds) {
    return isfinite(thresholds.switchOver) && isfinite(thresholds.backToWide) &&
            isfinite(thresholds.hysteresis) &&
            thresholds.switchOver > QCamera3SatPolicy::kMinZoomRatio &&
            thresholds.backToWide >= QCamera3SatPolicy::kMinZoomRatio &&
            thresholds.backToWide < thresholds.switchOver &&
            thresholds.hysteresis > 0.0f;
}

} // namespace

const char *QCamera3SatPolicy::reasonString(int32_t reason) {
    switch (reason) {
    case REASON_OK: return "ok";
    case REASON_INPUT: return "input";
    case REASON_PAIR: return "pair";
    case REASON_GEOMETRY: return "geometry";
    case REASON_ZOOM: return "zoom";
    case REASON_LOW_POWER: return "low-power";
    default: return "unknown";
    }
}

bool QCamera3SatPolicy::validCamera(const Camera &camera) {
    if (!camera.present) return false;
    if (!isfinite(camera.focalLengthMm) || camera.focalLengthMm <= 0.0f)
        return false;
    if (!isfinite(camera.horizontalFovDeg) || camera.horizontalFovDeg <= 0.0f ||
            camera.horizontalFovDeg >= 180.0f)
        return false;
    if (!isfinite(camera.verticalFovDeg) || camera.verticalFovDeg <= 0.0f ||
            camera.verticalFovDeg >= 180.0f)
        return false;
    if (camera.pixelWidth == 0 || camera.pixelHeight == 0) return false;
    return true;
}

bool QCamera3SatPolicy::thresholds(const Input &input, Thresholds &thresholds) {
    if (!validCamera(input.wide) || !validCamera(input.tele)) return false;
    if (!input.calibrationBound || !input.concurrentStreams) return false;
    if (input.tele.focalLengthMm <= input.wide.focalLengthMm) return false;
    if (input.tele.horizontalFovDeg >= input.wide.horizontalFovDeg) return false;
    if (!isfinite(input.maxDigitalZoom) ||
            input.maxDigitalZoom < kMinZoomRatio ||
            input.maxDigitalZoom > kMaxDigitalZoomLimit)
        return false;

    const float wideHalf = input.wide.horizontalFovDeg * 0.5f;
    const float teleHalf = input.tele.horizontalFovDeg * 0.5f;
    const float wideTan = tanf(wideHalf * kPi / 180.0f);
    const float teleTan = tanf(teleHalf * kPi / 180.0f);
    if (!isfinite(wideTan) || !isfinite(teleTan) || wideTan <= 0.0f ||
            teleTan <= 0.0f)
        return false;
    const float fovRatio = wideTan / teleTan;
    const float focalRatio = input.tele.focalLengthMm / input.wide.focalLengthMm;
    if (!isfinite(fovRatio) || !isfinite(focalRatio)) return false;
    if (fovRatio <= kMinSwitchRatio) return false;
    const float reference = fovRatio > focalRatio ? fovRatio : focalRatio;
    if (reference <= 0.0f) return false;
    const float disagreement = fabsf(fovRatio - focalRatio) / reference;
    if (!isfinite(disagreement) || disagreement > kCrossCheckTolerance)
        return false;
    // The optical switch point must be reachable inside the advertised digital
    // zoom range; otherwise the tele sensor could never become active.
    if (fovRatio > input.maxDigitalZoom) return false;

    const float hysteresis = kHysteresisFraction * (fovRatio - kMinZoomRatio);
    float backToWide = fovRatio - hysteresis;
    if (backToWide < kMinZoomRatio) backToWide = kMinZoomRatio;
    if (!(backToWide < fovRatio) || !(hysteresis > 0.0f)) return false;

    Thresholds candidate;
    candidate.switchOver = fovRatio;
    candidate.backToWide = backToWide;
    candidate.hysteresis = hysteresis;
    candidate.fovRatio = fovRatio;
    candidate.focalRatio = focalRatio;
    if (!validThresholds(candidate)) return false;
    thresholds = candidate;
    return true;
}

bool QCamera3SatPolicy::admitsSatMode(const Input &input,
        Thresholds &derived) {
    return thresholds(input, derived);
}

int32_t QCamera3SatPolicy::admit(const Input &input,
        const Thresholds &thresholds, float zoomRatio, int32_t currentRole,
        int32_t requestedLowPower, Decision &decision) {
    decision = Decision{};
    decision.zoomRatio = zoomRatio;
    if (!validCamera(input.wide) || !validCamera(input.tele)) {
        decision.status = -EINVAL;
        decision.reason = REASON_INPUT;
        return decision.status;
    }
    if (currentRole != ROLE_WIDE && currentRole != ROLE_TELE) {
        decision.status = -EINVAL;
        decision.reason = REASON_INPUT;
        return decision.status;
    }
    if (requestedLowPower != 0 && requestedLowPower != 1) {
        decision.status = -EINVAL;
        decision.reason = REASON_INPUT;
        return decision.status;
    }
    if (!input.calibrationBound || !input.concurrentStreams) {
        decision.status = -ENOTSUP;
        decision.reason = REASON_GEOMETRY;
        return decision.status;
    }
    if (!validThresholds(thresholds)) {
        decision.status = -ENOTSUP;
        decision.reason = REASON_PAIR;
        return decision.status;
    }
    if (!isfinite(zoomRatio)) {
        decision.status = -EINVAL;
        decision.reason = REASON_INPUT;
        return decision.status;
    }
    if (zoomRatio < kMinZoomRatio) {
        decision.status = -ERANGE;
        decision.reason = REASON_ZOOM;
        return decision.status;
    }
    if (!isfinite(input.maxDigitalZoom) || zoomRatio > input.maxDigitalZoom) {
        decision.status = -ERANGE;
        decision.reason = REASON_ZOOM;
        return decision.status;
    }

    int32_t role = currentRole;
    if (currentRole == ROLE_WIDE) {
        if (zoomRatio >= thresholds.switchOver) role = ROLE_TELE;
    } else {
        if (zoomRatio <= thresholds.backToWide) role = ROLE_WIDE;
    }

    int32_t lowPower = 0;
    if (requestedLowPower != 0) {
        int32_t applied = 0;
        const int32_t rc = admitLowPower(input, role, requestedLowPower, applied);
        if (rc != 0) {
            decision.status = rc;
            decision.reason = REASON_LOW_POWER;
            return decision.status;
        }
        lowPower = applied;
    }

    decision.status = 0;
    decision.reason = REASON_OK;
    decision.role = role;
    decision.switched = role != currentRole ? 1 : 0;
    decision.lowPower = lowPower;
    decision.thresholdMilli = milli(thresholds.switchOver);
    decision.detail[0] = milli(thresholds.backToWide);
    decision.detail[1] = milli(thresholds.hysteresis);
    return 0;
}

int32_t QCamera3SatPolicy::admit(const Input &input, float zoomRatio,
        int32_t currentRole, int32_t requestedLowPower, Decision &decision) {
    Thresholds thresholds;
    if (!QCamera3SatPolicy::thresholds(input, thresholds)) {
        decision = Decision{};
        decision.zoomRatio = zoomRatio;
        // Distinguish the two refusal causes without guessing a threshold.
        if (!validCamera(input.wide) || !validCamera(input.tele)) {
            decision.status = -EINVAL;
            decision.reason = REASON_INPUT;
        } else if (!input.calibrationBound || !input.concurrentStreams) {
            decision.status = -ENOTSUP;
            decision.reason = REASON_GEOMETRY;
        } else {
            decision.status = -ENOTSUP;
            decision.reason = REASON_PAIR;
        }
        return decision.status;
    }
    return admit(input, thresholds, zoomRatio, currentRole, requestedLowPower,
            decision);
}

bool QCamera3SatPolicy::lowPowerAvailable(const Input &input, int32_t role) {
    if (role == ROLE_WIDE) {
        // The tele sensor is the one being parked.
        return input.tele.lowPowerCapable != 0;
    }
    if (role == ROLE_TELE) {
        return input.wide.lowPowerCapable != 0;
    }
    return false;
}

int32_t QCamera3SatPolicy::admitLowPower(const Input &input, int32_t role,
        int32_t requested, int32_t &applied) {
    applied = 0;
    if (requested != 0 && requested != 1) return -EINVAL;
    if (role != ROLE_WIDE && role != ROLE_TELE) return -EINVAL;
    if (requested == 0) return 0; // explicit bypass, nothing applied
    if (!input.calibrationBound || !input.concurrentStreams) return -ENOTSUP;
    if (!lowPowerAvailable(input, role)) return -ENOTSUP;
    applied = 1;
    return 0;
}

} // namespace qcamera
