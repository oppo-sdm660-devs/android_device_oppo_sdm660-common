/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_LOGICAL_MULTICAM_H
#define QCAMERA3_LOGICAL_MULTICAM_H

#include <stddef.h>
#include <stdint.h>

namespace qcamera {

// Publication and request model for the standard Android logical multi-camera
// surface (camera device API 3.5):
//
//   logical id + physical ids       ANDROID_LOGICAL_MULTI_CAMERA_PHYSICAL_IDS
//                                   (byte list of null-terminated UTF-8 ids)
//   sync type                       ANDROID_LOGICAL_MULTI_CAMERA_SENSOR_SYNC_TYPE
//                                   (byte enum: APPROXIMATE 0, CALIBRATED 1)
//   active physical id (result)     ANDROID_LOGICAL_MULTI_CAMERA_ACTIVE_PHYSICAL_ID
//                                   (byte result string, a valid physical id)
//   physical request keys           ANDROID_REQUEST_AVAILABLE_PHYSICAL_CAMERA_REQUEST_KEYS
//                                   (int32 tag list, subset of available request keys)
//   zoom range                      ANDROID_CONTROL_ZOOM_RATIO_RANGE (float[2]) and
//                                   ANDROID_SCALER_AVAILABLE_MAX_DIGITAL_ZOOM (float)
//   capability                      ANDROID_REQUEST_AVAILABLE_CAPABILITIES_LOGICAL_MULTI_CAMERA
//
// Every input below is real device data (enumerated public ids, decoded lens and
// array records, the bound QCamera3DualGeometry profile, the driver's concurrent
// stream capability, the declared zoom/request-key tables). Nothing is inferred
// and no logical id is published when a condition cannot be met; the refusal
// leaves the existing standalone cameras and the paired ID-3 path untouched.
//
// Physical request admission follows the platform contract exactly:
//   * an unknown physical id fails process_capture_request with -EINVAL,
//   * an empty per-physical settings blob fails with -EINVAL,
//   * a key that is not in the advertised physical request key list is never
//     consumed by the HAL (ignored),
//   * a key that overlaps the session key list is rejected, because session
//     parameters can change dual-camera synchronization.
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
        uint32_t cameraId;        // existing standalone public camera id
        int32_t role;             // ROLE_WIDE or ROLE_TELE
        float focalLengthMm;      // decoded lens record
        float horizontalFovDeg;   // decoded lens record
        float verticalFovDeg;
        uint32_t pixelWidth, pixelHeight;      // full sensor resolution
        uint32_t activeArrayWidth, activeArrayHeight;
        int32_t mountAngleDeg;                 // decoded module record
        uint8_t standalonePublished;           // id remains enumerated on its own
        uint8_t syncCapable;                   // participates in the reported sync
    };

    struct Input {
        Physical physical[kMaxPhysical];
        size_t physicalCount;
        uint32_t logicalId;       // new id, not used by any standalone camera
        uint8_t logicalIdFree;
        uint8_t geometryBound;    // QCamera3DualGeometry profile bound to the pair
        uint8_t concurrentStreams; // both physical sensors can stream at once
        uint8_t hardwareSync;     // leader/follower start-of-exposure sync reported
        float maxDigitalZoom;     // ANDROID_SCALER_AVAILABLE_MAX_DIGITAL_ZOOM
        float zoomRatioMin;       // ANDROID_CONTROL_ZOOM_RATIO_RANGE
        float zoomRatioMax;
        uint32_t requestKeys[kMaxKeys]; // per-physical overridable tag ids
        size_t requestKeyCount;
        uint32_t sessionKeys[kMaxKeys]; // ANDROID_REQUEST_AVAILABLE_SESSION_KEYS
        size_t sessionKeyCount;
    };

    struct Plan {
        uint8_t publish;
        int32_t status;           // 0 when published, negative errno otherwise
        int32_t reason;           // Reason
        uint32_t logicalId;
        uint32_t physicalIds[kMaxPhysical];
        int32_t physicalRoles[kMaxPhysical];
        size_t physicalCount;
        int32_t syncType;         // SyncType
        float zoomRatioMin, zoomRatioMax;
        float maxDigitalZoom;
        uint32_t requestKeys[kMaxKeys];
        size_t requestKeyCount;
        uint32_t sessionKeys[kMaxKeys];
        size_t sessionKeyCount;
        Physical physical[kMaxPhysical]; // capabilities snapshot for the HWI
        int32_t activePhysicalIndex;     // initial active sensor (wide)
    };

    struct PhysicalRequest {
        const char *id;      // UTF-8 physical id as delivered by the framework
        size_t idLength;
        uint8_t hasSettings; // false means the settings blob was empty
        const uint32_t *keys;
        size_t keyCount;
    };

    enum KeyVerdict { KEY_APPLY = 0, KEY_IGNORE = 1, KEY_REJECT = 2 };

    static const char *name() { return "LogicalMultiCam"; }
    static const char *reasonString(int32_t reason);

    // Publish/refuse decision. On success plan.publish is 1 and plan.status 0.
    // On refusal plan.publish is 0, plan.status is a negative errno and
    // plan.reason explains it; the caller keeps today's camera list.
    static int32_t publish(const Input &input, Plan &plan);

    // Invariant: a published logical id must never take over or hide a
    // standalone physical camera id.
    static bool keepsStandaloneVisible(const Input &input, const Plan &plan);

    // Physical id <-> public id mapping. Ids are the decimal UTF-8 strings the
    // framework uses; strict parsing (digits only, no empty string, no leading
    // zero, bounded length).
    static bool encodeId(uint32_t publicId, char *text, size_t capacity,
            size_t &length);
    static bool decodeId(const char *text, size_t length, uint32_t &publicId);
    static int32_t physicalIndexForId(const Plan &plan, const char *text,
            size_t length, size_t &index);
    static bool publicIdForPhysical(const Plan &plan, size_t index,
            uint32_t &publicId);
    static bool publicIdForRole(const Plan &plan, int32_t role, uint32_t &publicId);

    // Per-physical capability snapshot the logical device must report for the
    // physical id, including the per-physical sync type when concurrent
    // physical streams from different sensors are supported.
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
    // Returns 0 when the physical request may be processed; the applied tag ids
    // are written to consumedKeys (never the ignored or rejected ones).
    // Failure: -EINVAL (unknown id, empty settings, rejected key, malformed
    // input), -ENOSPC (destination too small, nothing written), -EINVAL when the
    // request targets an unpublished plan.
    static int32_t admitPhysicalRequest(const Plan &plan,
            const PhysicalRequest &request, uint32_t *consumedKeys,
            size_t capacity, size_t &consumedCount);

    // Active physical camera for a zoom ratio. The threshold is the real
    // horizontal field-of-view ratio of the two decoded lenses; no hysteresis is
    // applied here because this reports what the running pipeline selected, not
    // what a new request should select (QCamera3SatPolicy owns switching).
    static bool activePhysicalId(const Plan &plan, float zoomRatio,
            uint32_t &publicId);
    static bool fovSwitchRatio(const Plan &plan, float &ratio);

    // Extent of the advertised zoom range, bounded by the real capability.
    static bool zoomRange(const Plan &plan, float &minimum, float &maximum);
};

} // namespace qcamera
#endif
