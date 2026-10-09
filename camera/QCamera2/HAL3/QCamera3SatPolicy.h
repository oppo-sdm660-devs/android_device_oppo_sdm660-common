/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_SAT_POLICY_H
#define QCAMERA3_SAT_POLICY_H

#include <stddef.h>
#include <stdint.h>

namespace qcamera {

// Smooth-zoom (SAT) admission between two simultaneously streamable cameras. The
// switch point comes from the decoded lens/FOV data: fovRatio = tan(wideHFov/2) /
// tan(teleHFov/2), cross-checked against the focal ratio, with a hysteresis band.
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

    // Below this optical separation the two sensors see essentially the same FOV.
    static constexpr float kMinSwitchRatio = 1.05f;
    // Hysteresis is this fraction of the optical separation (switchOver - 1).
    static constexpr float kHysteresisFraction = 0.10f;
    // The focal-ratio cross-check must agree with the FOV ratio within this tolerance.
    static constexpr float kCrossCheckTolerance = 0.25f;
    static constexpr float kMinZoomRatio = 1.0f;
    static constexpr float kMaxDigitalZoomLimit = 100.0f;

    struct Camera {
        uint8_t present;
        float focalLengthMm;
        float horizontalFovDeg;
        float verticalFovDeg;
        uint32_t pixelWidth, pixelHeight;
        // Decoded standby/low-power sensor mode; 0 = not reported.
        uint8_t lowPowerCapable;
    };

    struct Input {
        Camera wide;
        Camera tele;
        uint8_t calibrationBound;
        uint8_t concurrentStreams;
        float maxDigitalZoom;      // ANDROID_SCALER_AVAILABLE_MAX_DIGITAL_ZOOM
    };

    struct Thresholds {
        float switchOver;
        float backToWide;
        float hysteresis;  // switchOver - backToWide
        float fovRatio;
        float focalRatio;
    };

    struct Decision {
        int32_t status;     // 0 admitted; negative errno otherwise
        int32_t reason;
        int32_t role;       // active role after admission (valid when status == 0)
        int32_t switched;
        int32_t lowPower;
        float zoomRatio;
        int32_t thresholdMilli; // switchOver * 1000
        int32_t detail[2];      // {backToWide * 1000, hysteresis * 1000}
    };

    static const char *name() { return "SatSwitch"; }
    static const char *reasonString(int32_t reason);

    static bool validCamera(const Camera &camera);
    // Derives the switch point and band; false leaves thresholds untouched.
    static bool thresholds(const Input &input, Thresholds &thresholds);
    // Admission of the QTI SAT request control; returns the derived thresholds.
    static bool admitsSatMode(const Input &input, Thresholds &thresholds);

    // currentRole is the active sensor (ROLE_WIDE for the first request);
    // requestedLowPower is the decoded stream.lowPowerMode value (0/1).
    // On failure the caller must not change the active sensor or power state.
    static int32_t admit(const Input &input, const Thresholds &thresholds,
            float zoomRatio, int32_t currentRole, int32_t requestedLowPower,
            Decision &decision);
    static int32_t admit(const Input &input, float zoomRatio,
            int32_t currentRole, int32_t requestedLowPower, Decision &decision);

    // Used when lowPowerMode is present without a zoom update; role is the active sensor.
    static int32_t admitLowPower(const Input &input, int32_t role,
            int32_t requested, int32_t &applied);
    static bool lowPowerAvailable(const Input &input, int32_t role);
};

} // namespace qcamera
#endif
