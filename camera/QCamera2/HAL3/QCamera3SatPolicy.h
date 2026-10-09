/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_SAT_POLICY_H
#define QCAMERA3_SAT_POLICY_H

#include <stddef.h>
#include <stdint.h>

namespace qcamera {

// Smooth-zoom (SAT) admission between two real, simultaneously streamable
// cameras. The switch point is derived from the decoded lens/field-of-view data
// of both sensors, never from a fixed table:
//
//   fovRatio = tan(wideHFov / 2) / tan(teleHFov / 2)
//   focalRatio = teleFocal / wideFocal                 (independent cross-check)
//
// The tele camera becomes active at zoom ratio >= fovRatio and control returns
// to the wide camera at zoom ratio <= fovRatio - hysteresis, where the
// hysteresis band is a documented fraction of the optical separation. Inside the
// band the previously active sensor stays active, so a ratio that jitters around
// the switch point cannot ping-pong between sensors.
//
// What is *not* smooth, and is not claimed: there is no per-frame fusion, no
// stereo blending and no synthesized intermediate frame anywhere in this path.
// Only one physical sensor images at a time; the switch is a hand-off at a real
// optical threshold, and the frame produced immediately after the switch comes
// from the other sensor (with its own exposure, white balance and focus state).
// The hysteresis band limits how often that hand-off can happen; it does not
// make it seamless.
//
// Low-power mode parks the inactive sensor. It is admitted only when the parked
// camera reports a real low-power/standby capability, and is refused (status
// -ENOTSUP, applied 0) otherwise. Missing calibration/geometry, missing
// concurrent streaming, an unusable pair separation, or a zoom ratio outside the
// decoded range are all explicit refusals.
class QCamera3SatPolicy {
public:
    enum Role { ROLE_NONE = 0, ROLE_WIDE = 1, ROLE_TELE = 2 };
    enum Reason {
        REASON_OK = 0,
        REASON_INPUT,
        REASON_PAIR,
        REASON_GEOMETRY,
        REASON_ZOOM,
        REASON_LOW_POWER,
    };

    // A pair whose optical separation is below this ratio cannot be switched
    // usefully: the two sensors image essentially the same field of view.
    static constexpr float kMinSwitchRatio = 1.05f;
    // Hysteresis is this fraction of the optical separation (switchOver - 1).
    static constexpr float kHysteresisFraction = 0.10f;
    // The independent focal-ratio cross-check must agree with the FOV ratio
    // within this relative tolerance, otherwise the decoded geometry is
    // inconsistent and the switch point is refused instead of guessed.
    static constexpr float kCrossCheckTolerance = 0.25f;
    static constexpr float kMinZoomRatio = 1.0f;
    static constexpr float kMaxDigitalZoomLimit = 100.0f;

    struct Camera {
        uint8_t present;          // the standalone camera is enumerated
        float focalLengthMm;      // decoded lens record
        float horizontalFovDeg;
        float verticalFovDeg;
        uint32_t pixelWidth, pixelHeight;
        // Real report that this sensor can be parked while the other streams
        // (a decoded standby/low-power sensor mode). 0 means "not reported", and
        // then no low-power mode is advertised or applied.
        uint8_t lowPowerCapable;
    };

    struct Input {
        Camera wide;
        Camera tele;
        uint8_t calibrationBound;  // real dual calibration/geometry is bound
        uint8_t concurrentStreams; // both sensors can stream at the same time
        float maxDigitalZoom;      // ANDROID_SCALER_AVAILABLE_MAX_DIGITAL_ZOOM
    };

    struct Thresholds {
        float switchOver;  // ratio at which the tele takes over
        float backToWide;  // ratio below which the wide takes over again
        float hysteresis;  // switchOver - backToWide
        float fovRatio;    // raw field-of-view ratio
        float focalRatio;  // decoded focal ratio
    };

    struct Decision {
        int32_t status;     // 0 admitted; negative errno otherwise
        int32_t reason;     // Reason
        int32_t role;       // active role after admission (valid when status == 0)
        int32_t switched;   // 1 when this admission changes the active sensor
        int32_t lowPower;   // 1 when the low-power mode is applied
        float zoomRatio;
        int32_t thresholdMilli; // switchOver * 1000
        int32_t detail[2];      // {backToWide * 1000, hysteresis * 1000}
    };

    static const char *name() { return "SatSwitch"; }
    static const char *reasonString(int32_t reason);

    static bool validCamera(const Camera &camera);
    // Derives the switch point and its hysteresis band from the real decoded
    // geometry. False (thresholds untouched) when the pair is unusable.
    static bool thresholds(const Input &input, Thresholds &thresholds);
    // Admission of the QTI SAT request control: exactly the conditions that make
    // a real zoom switch possible, with the derived thresholds returned.
    static bool admitsSatMode(const Input &input, Thresholds &thresholds);

    // Zoom admission for one request. currentRole is the currently active
    // sensor (ROLE_WIDE for the first request). requestedLowPower is the decoded
    // org.lineageos.camera.stream.lowPowerMode value (0/1). On failure the
    // decision carries status < 0 and applied state 0; the caller must not
    // change the active sensor or the sensor power state.
    static int32_t admit(const Input &input, const Thresholds &thresholds,
            float zoomRatio, int32_t currentRole, int32_t requestedLowPower,
            Decision &decision);
    static int32_t admit(const Input &input, float zoomRatio,
            int32_t currentRole, int32_t requestedLowPower, Decision &decision);

    // Low-power admission alone (used when the vendor lowPowerMode control is
    // present without a zoom update). role is the active sensor; the parked
    // sensor must report the real capability.
    static int32_t admitLowPower(const Input &input, int32_t role,
            int32_t requested, int32_t &applied);
    static bool lowPowerAvailable(const Input &input, int32_t role);
};

} // namespace qcamera
#endif
