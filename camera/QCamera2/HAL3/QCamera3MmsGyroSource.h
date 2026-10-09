/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_MMS_GYRO_SOURCE_H
#define QCAMERA3_MMS_GYRO_SOURCE_H

#include <errno.h>
#include <stdint.h>

namespace qcamera {
class QCamera3MmsVstab;

// Framework sensorservice gyro client; timestamps are CLOCK_BOOTTIME ns, values
// rad/s in the Android sensor axes.
class QCamera3MmsGyroSource {
public:
    struct Identity {
        char name[128], vendor[128];
        int32_t handle, minDelayUs, maxDelayUs;
    };
    using Notify = void (*)(void *);

    static constexpr uint32_t kMaxListeners = 4;

    class Listener {
    public:
        virtual ~Listener() = default;
        // Same sample contract as QCamera3MmsVstab::addGyro; not called after stop().
        virtual int32_t onGyro(int64_t timestampNs, const float angularVelocity[3]) = 0;
        // Optional gravity; -ENOTSUP when not consumed.
        virtual int32_t onGravity(int64_t /*timestampNs*/, const float /*gravity*/[3]) {
            return -ENOTSUP;
        }
    };

    QCamera3MmsGyroSource();
    ~QCamera3MmsGyroSource();
    static int32_t probe(Identity &identity);
    // engine is configured before start; notify cannot call stop()/flush().
    int32_t start(QCamera3MmsVstab &engine, Notify notify, void *user);
    // Drains the active callback before clearing the engine pointers.
    void stop();
    // Registration survives flush()/start(); -ENOSPC or -EALREADY on failure.
    int32_t addListener(Listener *listener);
    int32_t removeListener(Listener *listener);
    bool started() const;
    // Enabled period in nanoseconds; 0 when no feed is running.
    int64_t periodNs() const;
    int32_t gravityAvailable() const;
    int32_t lastError() const;
    // Per-session feed statistics; kept after stop() and reset on the next start.
    struct Stats {
        uint64_t gyroEvents = 0;
        uint64_t engineDelivered = 0;
        uint64_t engineFailures = 0;
        int32_t lastEngineError = 0;
        int32_t lastError = 0;
    };
    bool getStats(Stats &stats) const;
private:
    struct Impl;
    Impl *mImpl;
    QCamera3MmsGyroSource(const QCamera3MmsGyroSource &) = delete;
    QCamera3MmsGyroSource &operator=(const QCamera3MmsGyroSource &) = delete;
};
} // namespace qcamera
#endif
