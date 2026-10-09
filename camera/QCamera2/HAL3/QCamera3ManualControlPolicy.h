/* Copyright (c) 2026. SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_MANUAL_CONTROL_POLICY_H
#define QCAMERA3_MANUAL_CONTROL_POLICY_H

#include <float.h>
#include <stddef.h>
#include <stdint.h>

namespace qcamera {

// Decoded sensor limits; the arithmetic is Android-independent for host tests.
struct QCamera3ManualControlPolicy {
    int64_t exposureMin;
    int64_t exposureMax;
    int64_t frameDurationMax;
    int32_t sensitivityMin;
    int32_t sensitivityMax;

    bool validRanges() const {
        return exposureMin > 0 && exposureMax >= exposureMin &&
                frameDurationMax >= exposureMax && sensitivityMin > 0 &&
                sensitivityMax >= sensitivityMin;
    }

    bool validExposure(int64_t value) const {
        return validRanges() && value >= exposureMin && value <= exposureMax;
    }

    bool validSensitivity(int32_t value) const {
        return validRanges() && value >= sensitivityMin && value <= sensitivityMax;
    }

    bool validFrameDuration(int64_t value) const {
        return validRanges() && value > 0 && value <= frameDurationMax;
    }

    static bool finiteNonnegative(float value) {
        return value >= 0.0f && value <= FLT_MAX;
    }

    static bool validGains(const float *gains, size_t count) {
        if (gains == NULL || count != 4) return false;
        for (size_t i = 0; i < count; ++i) {
            if (!(gains[i] > 0.0f) || gains[i] > FLT_MAX) return false;
        }
        return true;
    }
};

} // namespace qcamera
#endif
