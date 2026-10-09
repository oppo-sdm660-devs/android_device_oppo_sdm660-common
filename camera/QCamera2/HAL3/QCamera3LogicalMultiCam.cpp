/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3LogicalMultiCam.h"

#include <errno.h>
#include <math.h>
#include <string.h>

namespace qcamera {
namespace {

using Policy = QCamera3LogicalMultiCam;

constexpr float kPi = 3.14159265358979323846f;

bool validKeyList(const uint32_t *keys, size_t count) {
    if (!keys || count == 0 || count > Policy::kMaxKeys) return false;
    for (size_t i = 0; i < count; ++i) {
        if (keys[i] == 0) return false;
        for (size_t j = i + 1; j < count; ++j)
            if (keys[i] == keys[j]) return false;
    }
    return true;
}

bool listsOverlap(const uint32_t *first, size_t firstCount,
        const uint32_t *second, size_t secondCount) {
    for (size_t i = 0; i < firstCount; ++i)
        for (size_t j = 0; j < secondCount; ++j)
            if (first[i] == second[j]) return true;
    return false;
}

bool validPhysical(const Policy::Physical &physical) {
    if (physical.cameraId > Policy::kMaxPublicId) return false;
    if (physical.role != Policy::ROLE_WIDE && physical.role != Policy::ROLE_TELE)
        return false;
    if (!isfinite(physical.focalLengthMm) || physical.focalLengthMm <= 0.0f)
        return false;
    if (!isfinite(physical.horizontalFovDeg) ||
            physical.horizontalFovDeg <= 0.0f ||
            physical.horizontalFovDeg >= 180.0f)
        return false;
    if (!isfinite(physical.verticalFovDeg) || physical.verticalFovDeg <= 0.0f ||
            physical.verticalFovDeg >= 180.0f)
        return false;
    if (physical.pixelWidth == 0 || physical.pixelHeight == 0) return false;
    if (physical.activeArrayWidth == 0 || physical.activeArrayHeight == 0)
        return false;
    if (physical.mountAngleDeg < 0 || physical.mountAngleDeg >= 360) return false;
    return true;
}

} // namespace

const char *QCamera3LogicalMultiCam::reasonString(int32_t reason) {
    switch (reason) {
    case REASON_OK: return "ok";
    case REASON_PHYSICAL_COUNT: return "physical-count";
    case REASON_PHYSICAL_MISSING: return "physical-missing";
    case REASON_PHYSICAL_GEOMETRY: return "physical-geometry";
    case REASON_ROLE_MISMATCH: return "role-mismatch";
    case REASON_SYNC_MISSING: return "sync-missing";
    case REASON_CONCURRENCY_MISSING: return "concurrency-missing";
    case REASON_GEOMETRY_UNBOUND: return "geometry-unbound";
    case REASON_LOGICAL_ID_BUSY: return "logical-id-busy";
    case REASON_ZOOM_RANGE: return "zoom-range";
    case REASON_REQUEST_KEYS: return "request-keys";
    default: return "unknown";
    }
}

int32_t QCamera3LogicalMultiCam::publish(const Input &input, Plan &plan) {
    // Refusal leaves the caller-visible plan empty (publish == 0).
    plan = Plan{};
    if (input.physicalCount != kMaxPhysical) {
        plan.status = -EINVAL;
        plan.reason = REASON_PHYSICAL_COUNT;
        return plan.status;
    }
    for (size_t i = 0; i < input.physicalCount; ++i) {
        const Physical &physical = input.physical[i];
        if (!physical.standalonePublished) {
            plan.status = -ENODEV;
            plan.reason = REASON_PHYSICAL_MISSING;
            return plan.status;
        }
        if (!validPhysical(physical)) {
            plan.status = -EINVAL;
            plan.reason = REASON_PHYSICAL_GEOMETRY;
            return plan.status;
        }
    }

    size_t wideSearch = input.physicalCount, teleSearch = input.physicalCount;
    for (size_t i = 0; i < input.physicalCount; ++i) {
        if (input.physical[i].role == ROLE_WIDE) wideSearch = i;
        else if (input.physical[i].role == ROLE_TELE) teleSearch = i;
    }
    if (wideSearch >= input.physicalCount || teleSearch >= input.physicalCount ||
            wideSearch == teleSearch) {
        plan.status = -EINVAL;
        plan.reason = REASON_ROLE_MISMATCH;
        return plan.status;
    }
    const Physical &wide = input.physical[wideSearch];
    const Physical &tele = input.physical[teleSearch];
    if (tele.focalLengthMm <= wide.focalLengthMm ||
            tele.horizontalFovDeg >= wide.horizontalFovDeg) {
        plan.status = -EINVAL;
        plan.reason = REASON_ROLE_MISMATCH;
        return plan.status;
    }

    if (!wide.syncCapable || !tele.syncCapable) {
        plan.status = -ENOTSUP;
        plan.reason = REASON_SYNC_MISSING;
        return plan.status;
    }
    if (!input.concurrentStreams) {
        plan.status = -ENOTSUP;
        plan.reason = REASON_CONCURRENCY_MISSING;
        return plan.status;
    }
    if (!input.geometryBound) {
        plan.status = -ENOTSUP;
        plan.reason = REASON_GEOMETRY_UNBOUND;
        return plan.status;
    }

    if (!input.logicalIdFree || input.logicalId > kMaxPublicId) {
        plan.status = -EEXIST;
        plan.reason = REASON_LOGICAL_ID_BUSY;
        return plan.status;
    }
    for (size_t i = 0; i < input.physicalCount; ++i) {
        if (input.physical[i].cameraId == input.logicalId) {
            plan.status = -EEXIST;
            plan.reason = REASON_LOGICAL_ID_BUSY;
            return plan.status;
        }
    }

    if (!isfinite(input.maxDigitalZoom) || input.maxDigitalZoom < kMinZoomRatio ||
            !isfinite(input.zoomRatioMin) || !isfinite(input.zoomRatioMax) ||
            input.zoomRatioMin < kMinZoomRatio ||
            input.zoomRatioMax < input.zoomRatioMin ||
            input.zoomRatioMax > input.maxDigitalZoom) {
        plan.status = -ERANGE;
        plan.reason = REASON_ZOOM_RANGE;
        return plan.status;
    }

    if (!validKeyList(input.requestKeys, input.requestKeyCount) ||
            (input.sessionKeyCount != 0 &&
             !validKeyList(input.sessionKeys, input.sessionKeyCount)) ||
            (input.sessionKeyCount != 0 &&
             listsOverlap(input.requestKeys, input.requestKeyCount,
                     input.sessionKeys, input.sessionKeyCount))) {
        plan.status = -EINVAL;
        plan.reason = REASON_REQUEST_KEYS;
        return plan.status;
    }

    plan.publish = 1;
    plan.status = 0;
    plan.reason = REASON_OK;
    plan.logicalId = input.logicalId;
    plan.physicalCount = input.physicalCount;
    for (size_t i = 0; i < input.physicalCount; ++i) {
        plan.physical[i] = input.physical[i];
        plan.physicalIds[i] = input.physical[i].cameraId;
        plan.physicalRoles[i] = input.physical[i].role;
    }
    // CALIBRATED only when the driver reports hardware sync, else APPROXIMATE.
    plan.syncType = input.hardwareSync ? SYNC_CALIBRATED : SYNC_APPROXIMATE;
    plan.zoomRatioMin = input.zoomRatioMin;
    plan.zoomRatioMax = input.zoomRatioMax;
    plan.maxDigitalZoom = input.maxDigitalZoom;
    memcpy(plan.requestKeys, input.requestKeys,
            input.requestKeyCount * sizeof(uint32_t));
    plan.requestKeyCount = input.requestKeyCount;
    if (input.sessionKeyCount != 0) {
        memcpy(plan.sessionKeys, input.sessionKeys,
                input.sessionKeyCount * sizeof(uint32_t));
        plan.sessionKeyCount = input.sessionKeyCount;
    }
    plan.activePhysicalIndex = int32_t(wideSearch);
    return 0;
}

bool QCamera3LogicalMultiCam::keepsStandaloneVisible(const Input &input,
        const Plan &plan) {
    if (!plan.publish) return true;
    if (plan.physicalCount != input.physicalCount) return false;
    for (size_t i = 0; i < plan.physicalCount; ++i) {
        if (!plan.physical[i].standalonePublished) return false;
        if (plan.physical[i].cameraId == plan.logicalId) return false;
    }
    return true;
}

bool QCamera3LogicalMultiCam::encodeId(uint32_t publicId, char *text,
        size_t capacity, size_t &length) {
    length = 0;
    if (!text || capacity == 0 || publicId > kMaxPublicId) return false;
    char digits[kMaxIdLength];
    size_t count = 0;
    uint32_t value = publicId;
    do {
        digits[count++] = char('0' + (value % 10));
        value /= 10;
    } while (value != 0 && count < kMaxIdLength);
    if (value != 0 || capacity < count + 1) return false;
    for (size_t i = 0; i < count; ++i) text[i] = digits[count - 1 - i];
    text[count] = '\0';
    length = count;
    return true;
}

bool QCamera3LogicalMultiCam::decodeId(const char *text, size_t length,
        uint32_t &publicId) {
    publicId = 0;
    if (!text || length == 0 || length > kMaxIdLength) return false;
    if (length > 1 && text[0] == '0') return false;
    uint32_t value = 0;
    for (size_t i = 0; i < length; ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        value = value * 10 + uint32_t(text[i] - '0');
    }
    if (value > kMaxPublicId) return false;
    publicId = value;
    return true;
}

int32_t QCamera3LogicalMultiCam::physicalIndexForId(const Plan &plan,
        const char *text, size_t length, size_t &index) {
    index = 0;
    if (!plan.publish) return -EINVAL;
    uint32_t publicId = 0;
    if (!decodeId(text, length, publicId)) return -EINVAL;
    for (size_t i = 0; i < plan.physicalCount; ++i) {
        if (plan.physicalIds[i] == publicId) {
            index = i;
            return 0;
        }
    }
    return -EINVAL;
}

bool QCamera3LogicalMultiCam::publicIdForPhysical(const Plan &plan, size_t index,
        uint32_t &publicId) {
    publicId = 0;
    if (!plan.publish || index >= plan.physicalCount) return false;
    publicId = plan.physicalIds[index];
    return true;
}

bool QCamera3LogicalMultiCam::publicIdForRole(const Plan &plan, int32_t role,
        uint32_t &publicId) {
    publicId = 0;
    if (!plan.publish || (role != ROLE_WIDE && role != ROLE_TELE)) return false;
    for (size_t i = 0; i < plan.physicalCount; ++i) {
        if (plan.physicalRoles[i] == role) {
            publicId = plan.physicalIds[i];
            return true;
        }
    }
    return false;
}

bool QCamera3LogicalMultiCam::physicalCapabilities(const Plan &plan, size_t index,
        PhysicalCapabilities &capabilities) {
    capabilities = PhysicalCapabilities{};
    if (!plan.publish || index >= plan.physicalCount) return false;
    const Physical &physical = plan.physical[index];
    capabilities.cameraId = physical.cameraId;
    capabilities.role = physical.role;
    capabilities.focalLengthMm = physical.focalLengthMm;
    capabilities.horizontalFovDeg = physical.horizontalFovDeg;
    capabilities.verticalFovDeg = physical.verticalFovDeg;
    capabilities.pixelWidth = physical.pixelWidth;
    capabilities.pixelHeight = physical.pixelHeight;
    capabilities.activeArrayWidth = physical.activeArrayWidth;
    capabilities.activeArrayHeight = physical.activeArrayHeight;
    capabilities.mountAngleDeg = physical.mountAngleDeg;
    capabilities.syncType = plan.syncType;
    return true;
}

QCamera3LogicalMultiCam::KeyVerdict QCamera3LogicalMultiCam::admitPhysicalKey(
        uint32_t tag, const uint32_t *advertised, size_t advertisedCount,
        const uint32_t *sessionKeys, size_t sessionCount) {
    if (tag == 0) return KEY_REJECT; // not a metadata tag id
    for (size_t i = 0; i < sessionCount; ++i)
        if (sessionKeys && sessionKeys[i] == tag) return KEY_REJECT;
    for (size_t i = 0; i < advertisedCount; ++i)
        if (advertised && advertised[i] == tag) return KEY_APPLY;
    return KEY_IGNORE;
}

QCamera3LogicalMultiCam::KeyVerdict QCamera3LogicalMultiCam::admitPhysicalKey(
        uint32_t tag, const Plan &plan) {
    return admitPhysicalKey(tag, plan.requestKeys, plan.requestKeyCount,
            plan.sessionKeys, plan.sessionKeyCount);
}

int32_t QCamera3LogicalMultiCam::admitPhysicalRequest(const Plan &plan,
        const PhysicalRequest &request, uint32_t *consumedKeys, size_t capacity,
        size_t &consumedCount) {
    consumedCount = 0;
    if (!plan.publish) return -EINVAL;
    if (!request.id || request.idLength == 0 ||
            request.idLength > kMaxIdLength) return -EINVAL;
    // Framework contract: unknown physical id or empty settings fails the request.
    if (!request.hasSettings) return -EINVAL;
    if (request.keyCount != 0 && !request.keys) return -EINVAL;
    if (request.keyCount > kMaxKeys) return -EINVAL;
    size_t index = 0;
    if (physicalIndexForId(plan, request.id, request.idLength, index) != 0)
        return -EINVAL;
    for (size_t i = 0; i < request.keyCount; ++i)
        for (size_t j = i + 1; j < request.keyCount; ++j)
            if (request.keys[i] == request.keys[j]) return -EINVAL;

    // Verdict pass first, so a rejected key fails the request atomically.
    size_t applied = 0;
    for (size_t i = 0; i < request.keyCount; ++i) {
        const KeyVerdict verdict = admitPhysicalKey(request.keys[i], plan);
        if (verdict == KEY_REJECT) return -EINVAL;
        if (verdict == KEY_APPLY) ++applied;
    }
    if (applied == 0) return 0; // everything ignored: nothing is consumed
    if (!consumedKeys || capacity < applied) return -ENOSPC;
    size_t written = 0;
    for (size_t i = 0; i < request.keyCount; ++i)
        if (admitPhysicalKey(request.keys[i], plan) == KEY_APPLY)
            consumedKeys[written++] = request.keys[i];
    consumedCount = written;
    return 0;
}

bool QCamera3LogicalMultiCam::fovSwitchRatio(const Plan &plan, float &ratio) {
    ratio = 0.0f;
    if (!plan.publish) return false;
    size_t wide = plan.physicalCount, tele = plan.physicalCount;
    for (size_t i = 0; i < plan.physicalCount; ++i) {
        if (plan.physicalRoles[i] == ROLE_WIDE) wide = i;
        else if (plan.physicalRoles[i] == ROLE_TELE) tele = i;
    }
    if (wide >= plan.physicalCount || tele >= plan.physicalCount) return false;
    const float wideHalf = plan.physical[wide].horizontalFovDeg * 0.5f;
    const float teleHalf = plan.physical[tele].horizontalFovDeg * 0.5f;
    const float wideTan = tanf(wideHalf * kPi / 180.0f);
    const float teleTan = tanf(teleHalf * kPi / 180.0f);
    if (!isfinite(wideTan) || !isfinite(teleTan) || teleTan <= 0.0f ||
            wideTan <= 0.0f) return false;
    const float value = wideTan / teleTan;
    if (!isfinite(value) || value <= kMinZoomRatio) return false;
    ratio = value;
    return true;
}

bool QCamera3LogicalMultiCam::activePhysicalId(const Plan &plan, float zoomRatio,
        uint32_t &publicId) {
    publicId = 0;
    if (!plan.publish || !isfinite(zoomRatio) || zoomRatio < kMinZoomRatio)
        return false;
    float ratio = 0.0f;
    if (!fovSwitchRatio(plan, ratio)) return false;
    return publicIdForRole(plan, zoomRatio < ratio ? ROLE_WIDE : ROLE_TELE,
            publicId);
}

bool QCamera3LogicalMultiCam::zoomRange(const Plan &plan, float &minimum,
        float &maximum) {
    minimum = 0.0f;
    maximum = 0.0f;
    if (!plan.publish) return false;
    minimum = plan.zoomRatioMin;
    maximum = plan.zoomRatioMax;
    return true;
}

} // namespace qcamera
