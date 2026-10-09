/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_DUAL_GEOMETRY_H
#define QCAMERA3_DUAL_GEOMETRY_H

#include "QCamera3DualCapture.h"

namespace qcamera {

// Binds unit calibration to sensor capabilities and the backend's CPP crop coordinates.
class QCamera3DualGeometry {
public:
    using Rect = QCamera3DualCapture::Rect;
    using Calibration = QCamera3DualCapture::Calibration;
    static constexpr size_t kFileSize = 352;
    struct SensorInfo {
        uint32_t pixelWidth, pixelHeight;
        Rect activeArray;
        uint32_t mountAngle;
        float focalLength, horizontalFov, verticalFov;
    };
    struct SensorProfile {
        SensorInfo info;
        uint32_t outputWidth, outputHeight;
        QCamera3DualCapture::Sensor capture;
    };
    struct Profile {
        Calibration::Source calibrationSource = Calibration::NONE;
        uint8_t calibrationSha256[32] = {};
        // Tooling report digest, kept for profile-file compatibility.
        uint8_t evidenceSha256[32] = {};
        SensorProfile sensors[2] = {};
        int64_t maxTimestampDeltaNs = 0, timeoutNs = 0;
        bool runtimeGeometry = false; // Derived in memory, never deserialized.
    };
    static const char *installedPath();
    static int32_t load(const Calibration &, Profile &);
    static int32_t load(const char *path, const Calibration &, Profile &);
    static int32_t fromCapabilities(const Calibration &, const SensorInfo sensors[2], Profile &);
    // Byte parser; load() also enforces installed-profile permissions.
    static int32_t decode(const uint8_t *data, size_t length, const Calibration &, Profile &);
    static bool matchesCalibration(const Profile &, const Calibration &);
    static bool matchesCapabilities(const Profile &, const SensorInfo sensors[2]);
    static int32_t bind(const Profile &, const Calibration &, const SensorInfo sensors[2],
            const uint32_t handles[2], const uint32_t widths[2], const uint32_t heights[2],
            QCamera3DualCapture::Config &);
};
} // namespace qcamera
#endif
