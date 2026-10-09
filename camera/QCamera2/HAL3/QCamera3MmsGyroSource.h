/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_MMS_GYRO_SOURCE_H
#define QCAMERA3_MMS_GYRO_SOURCE_H

#include <errno.h>
#include <stdint.h>

namespace qcamera {
class QCamera3MmsVstab;

// Framework sensorservice client, independent of the HAL1 OPPO sensor bridge.
// Event timestamps are nanoseconds in CLOCK_BOOTTIME; values are rad/s in the
// Android sensor axes. The selected MMS profile owns sensor-to-camera rotation.
//
// Fan-out seam: the engine passed to start() is one consumer; any number of
// bounded Listener registrations receive the very same sample (no copy, no
// reinterpretation, no smoothing). A listener refusal is counted and never
// changes the sample the other consumers see. The 2026-10-17 B2-c batch
// deleted the only registration site (the continuous stage graph), so this
// port currently registers no listener; the seam itself is kept because it is
// part of the EIS/MMS chain.
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
        // Real sample, identical semantics to QCamera3MmsVstab::addGyro:
        // strictly increasing CLOCK_BOOTTIME nanoseconds, finite rad/s in the
        // Android sensor axes. Never called after stop() returned.
        virtual int32_t onGyro(int64_t timestampNs, const float angularVelocity[3]) = 0;
        // Optional real gravity (TYPE_GRAVITY, or the calibrated accelerometer
        // when the unit has no fused gravity sensor). -ENOTSUP when the
        // listener has no gravity input at all.
        virtual int32_t onGravity(int64_t /*timestampNs*/, const float /*gravity*/[3]) {
            return -ENOTSUP;
        }
    };

    QCamera3MmsGyroSource();
    ~QCamera3MmsGyroSource();
    static int32_t probe(Identity &identity);
    // engine must already be configured. A nonblocking notify may wake the
    // channel worker; it must not call stop() or flush() from the callback.
    int32_t start(QCamera3MmsVstab &engine, Notify notify, void *user);
    // Drains any active callback before clearing its engine/user pointers.
    // Stop the feed before destroying or reconfiguring its engine.
    void stop();
    // Registration belongs to the session, not to one start(): a registered
    // listener keeps receiving samples across flush()/start() and is only
    // detached by removeListener()/the destructor. -ENOSPC when the bounded
    // table is full, -EALREADY for a duplicate registration.
    int32_t addListener(Listener *listener);
    int32_t removeListener(Listener *listener);
    bool started() const;
    // Real enabled period in nanoseconds, 0 when no feed is running.
    int64_t periodNs() const;
    // 1 only when a real gravity/accelerometer sensor was found AND enabled.
    int32_t gravityAvailable() const;
    int32_t lastError() const;
private:
    struct Impl;
    Impl *mImpl;
    QCamera3MmsGyroSource(const QCamera3MmsGyroSource &) = delete;
    QCamera3MmsGyroSource &operator=(const QCamera3MmsGyroSource &) = delete;
};
} // namespace qcamera
#endif
