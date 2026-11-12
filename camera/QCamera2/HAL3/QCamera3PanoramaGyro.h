/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_PANORAMA_GYRO_H
#define QCAMERA3_PANORAMA_GYRO_H

#include "QCamera3OppoPanorama.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <errno.h>
#include <mutex>

namespace qcamera {

// Sensorservice and camera source timestamps use CLOCK_BOOTTIME nanoseconds.
class QCamera3PanoramaGyro {
public:
    void reset(int32_t sensorMountAngle) {
        std::lock_guard<std::mutex> lock(mutex_);
        samples_.clear();
        mountAngle_ = sensorMountAngle;
    }

    int32_t add(int64_t timestampNs, const float angularVelocity[3]) {
        if (timestampNs <= 0 || angularVelocity == nullptr ||
                !std::isfinite(angularVelocity[0]) || !std::isfinite(angularVelocity[1]) ||
                !std::isfinite(angularVelocity[2])) return -EINVAL;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!samples_.empty() && timestampNs <= samples_.back().timestampNs) return -EINVAL;
        samples_.push_back({timestampNs, {angularVelocity[0], angularVelocity[1]}});
        if (samples_.size() > 512) samples_.pop_front();
        return 0;
    }

    QCamera3OppoPanorama::GyroSample interval(int64_t fromNs, int64_t toNs) const {
        QCamera3OppoPanorama::GyroSample output;
        output.timestampUs = toNs / 1000;
        std::lock_guard<std::mutex> lock(mutex_);
        if (fromNs >= toNs || samples_.size() < 2 || fromNs < samples_.front().timestampNs ||
                toNs > samples_.back().timestampNs) return output;
        std::array<double, 2> angle = {};
        for (size_t n = 1; n < samples_.size(); ++n) {
            const auto &a = samples_[n - 1];
            const auto &b = samples_[n];
            if (b.timestampNs <= fromNs) continue;
            if (a.timestampNs >= toNs) break;
            // A missing sensor segment is image-only, never a held last sample.
            if (b.timestampNs - a.timestampNs > 50000000) return output;
            const int64_t begin = std::max(fromNs, a.timestampNs);
            const int64_t end = std::min(toNs, b.timestampNs);
            const double u = double(begin - a.timestampNs) / (b.timestampNs - a.timestampNs);
            const double v = double(end - a.timestampNs) / (b.timestampNs - a.timestampNs);
            for (size_t axis = 0; axis < angle.size(); ++axis) {
                const double difference = b.velocity[axis] - a.velocity[axis];
                angle[axis] += (a.velocity[axis] + difference * (u + v) / 2.0) *
                        double(end - begin) / 1e9;
            }
        }
        constexpr double radiansToDegrees = 180.0 / 3.14159265358979323846;
        const float pitch = float(angle[0] * radiansToDegrees);
        const float yaw = float(angle[1] * radiansToDegrees);
        // Canvas placement opposes image motion; undo the rear sensor mounting.
        switch (mountAngle_) {
            case 0: output.angleXDeg = -yaw; output.angleYDeg = -pitch; break;
            case 90: output.angleXDeg = -pitch; output.angleYDeg = yaw; break;
            case 180: output.angleXDeg = yaw; output.angleYDeg = pitch; break;
            case 270: output.angleXDeg = pitch; output.angleYDeg = -yaw; break;
            default: return output;
        }
        output.valid = true;
        return output;
    }

private:
    struct Sample { int64_t timestampNs; std::array<float, 2> velocity; };
    mutable std::mutex mutex_;
    std::deque<Sample> samples_;
    int32_t mountAngle_ = 0;
};

} // namespace qcamera
#endif
