/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_SENSOR_HDR_POLICY_H
#define QCAMERA3_SENSOR_HDR_POLICY_H

#include <cstddef>
#include <cstdint>

namespace qcamera {
// Stock setSensorHDR tests bits 15/35/36 and sends 1/2/3 in the 4-byte record.
class QCamera3SensorHdrPolicy {
public:
    static constexpr uint64_t kInSensor = uint64_t(1) << 15;
    static constexpr uint64_t kZigzag = uint64_t(1) << 35;
    static constexpr uint64_t kStaggered = uint64_t(1) << 36;
    static constexpr uint64_t kMask = kInSensor | kZigzag | kStaggered;
    static int32_t select(uint64_t reportedFeatures) {
        if (reportedFeatures & kInSensor) return 1;
        if (reportedFeatures & kZigzag) return 2;
        if (reportedFeatures & kStaggered) return 3;
        return 0;
    }
    static uint64_t feature(int32_t type) {
        return type == 1 ? kInSensor : type == 2 ? kZigzag :
                type == 3 ? kStaggered : 0;
    }
    static bool output(uint32_t width, uint32_t height) {
        return width >= 32 && height >= 32 && !((width | height) & 1) &&
                width <= 1920 && height <= 1080;
    }
    static bool request(int32_t mode, bool available, bool normalVideo,
            bool automaticExposure, int32_t maxFps, size_t outputs) {
        return mode == 0 || (mode == 1 && available && normalVideo &&
                automaticExposure && maxFps > 0 && maxFps <= 30 &&
                outputs >= 1 && outputs <= 2);
    }
    static bool stable(bool chosen, int32_t previous, int32_t requested) {
        return !chosen || previous == requested;
    }
};
} // namespace qcamera
#endif
