/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_LOGICAL_MULTICAM_H
#define QCAMERA3_LOGICAL_MULTICAM_H

#include <stddef.h>
#include <stdint.h>

namespace qcamera {

// Publication/request model for the logical multi-camera surface (device API 3.5);
// refusals leave the existing standalone cameras untouched.
class QCamera3LogicalMultiCam {
public:
    static constexpr size_t kMaxPhysical = 2;
    static constexpr size_t kMaxKeys = 16;
    static constexpr size_t kMaxIdLength = 3; // "255"
    static constexpr uint32_t kMaxPublicId = 255;
    static constexpr float kMinZoomRatio = 1.0f;
    static constexpr float kFovCompareEpsilon = 0.001f;

    enum Role { ROLE_NONE = 0, ROLE_WIDE = 1, ROLE_TELE = 2 };
    // Wire values of ANDROID_LOGICAL_MULTI_CAMERA_SENSOR_SYNC_TYPE.
    enum SyncType { SYNC_APPROXIMATE = 0, SYNC_CALIBRATED = 1 };

    enum Reason {
        REASON_OK = 0,
        REASON_PHYSICAL_COUNT,
        REASON_PHYSICAL_MISSING,
        REASON_PHYSICAL_GEOMETRY,
        REASON_ROLE_MISMATCH,
        REASON_SYNC_MISSING,
        REASON_CONCURRENCY_MISSING,
        REASON_GEOMETRY_UNBOUND,
        REASON_LOGICAL_ID_BUSY,
        REASON_ZOOM_RANGE,
        REASON_REQUEST_KEYS,
    };

    struct Physical {
        uint32_t cameraId;
        int32_t role;
        float focalLengthMm;
        float horizontalFovDeg;
        float verticalFovDeg;
        uint32_t pixelWidth, pixelHeight;
        uint32_t activeArrayWidth, activeArrayHeight;
        int32_t mountAngleDeg;
        uint8_t standalonePublished;
        uint8_t syncCapable;
    };

    struct Input {
        Physical physical[kMaxPhysical];
        size_t physicalCount;
        uint32_t logicalId;
        uint8_t logicalIdFree;
        uint8_t geometryBound;
        uint8_t concurrentStreams;
        uint8_t hardwareSync;
        float maxDigitalZoom;     // ANDROID_SCALER_AVAILABLE_MAX_DIGITAL_ZOOM
        float zoomRatioMin;       // ANDROID_CONTROL_ZOOM_RATIO_RANGE
        float zoomRatioMax;
        uint32_t requestKeys[kMaxKeys];
        size_t requestKeyCount;
        uint32_t sessionKeys[kMaxKeys]; // ANDROID_REQUEST_AVAILABLE_SESSION_KEYS
        size_t sessionKeyCount;
    };

    struct Plan {
        uint8_t publish;
        int32_t status;           // 0 when published, negative errno otherwise
        int32_t reason;
        uint32_t logicalId;
        uint32_t physicalIds[kMaxPhysical];
        int32_t physicalRoles[kMaxPhysical];
        size_t physicalCount;
        int32_t syncType;
        float zoomRatioMin, zoomRatioMax;
        float maxDigitalZoom;
        uint32_t requestKeys[kMaxKeys];
        size_t requestKeyCount;
        uint32_t sessionKeys[kMaxKeys];
        size_t sessionKeyCount;
        Physical physical[kMaxPhysical];
        int32_t activePhysicalIndex;
    };

    struct PhysicalRequest {
        const char *id;
        size_t idLength;
        uint8_t hasSettings; // false means the settings blob was empty
        const uint32_t *keys;
        size_t keyCount;
    };

    enum KeyVerdict { KEY_APPLY = 0, KEY_IGNORE = 1, KEY_REJECT = 2 };

    static const char *name() { return "LogicalMultiCam"; }
    static const char *reasonString(int32_t reason);

    // On success publish is 1 and status 0; on refusal publish is 0, status a
    // negative errno and reason explains it.
    static int32_t publish(const Input &input, Plan &plan);

    // A published logical id never hides a standalone physical camera id.
    static bool keepsStandaloneVisible(const Input &input, const Plan &plan);

    // Decimal UTF-8 ids; strict parsing (digits only, no empty, no leading zero).
    static bool encodeId(uint32_t publicId, char *text, size_t capacity,
            size_t &length);
    static bool decodeId(const char *text, size_t length, uint32_t &publicId);
    static int32_t physicalIndexForId(const Plan &plan, const char *text,
            size_t length, size_t &index);
    static bool publicIdForPhysical(const Plan &plan, size_t index,
            uint32_t &publicId);
    static bool publicIdForRole(const Plan &plan, int32_t role, uint32_t &publicId);

    // Capability snapshot the logical device reports for the physical id.
    struct PhysicalCapabilities {
        uint32_t cameraId;
        int32_t role;
        float focalLengthMm, horizontalFovDeg, verticalFovDeg;
        uint32_t pixelWidth, pixelHeight;
        uint32_t activeArrayWidth, activeArrayHeight;
        int32_t mountAngleDeg;
        int32_t syncType;
    };
    static bool physicalCapabilities(const Plan &plan, size_t index,
            PhysicalCapabilities &capabilities);

    // Request admission.
    static KeyVerdict admitPhysicalKey(uint32_t tag, const uint32_t *advertised,
            size_t advertisedCount, const uint32_t *sessionKeys,
            size_t sessionCount);
    static KeyVerdict admitPhysicalKey(uint32_t tag, const Plan &plan);
    // Returns 0 when processable; consumedKeys receives only applied tag ids.
    // -EINVAL for unknown id / empty settings / rejected key / malformed input;
    // -ENOSPC when the destination is too small (nothing is written).
    static int32_t admitPhysicalRequest(const Plan &plan,
            const PhysicalRequest &request, uint32_t *consumedKeys,
            size_t capacity, size_t &consumedCount);

    // Threshold is the real horizontal field-of-view ratio of the two lenses; no
    // hysteresis here (QCamera3SatPolicy owns switching).
    static bool activePhysicalId(const Plan &plan, float zoomRatio,
            uint32_t &publicId);
    static bool fovSwitchRatio(const Plan &plan, float &ratio);

    static bool zoomRange(const Plan &plan, float &minimum, float &maximum);
};

} // namespace qcamera
#endif
