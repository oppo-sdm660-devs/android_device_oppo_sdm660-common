/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_MMS_VSTAB_H
#define QCAMERA3_MMS_VSTAB_H

#include "QCamera3HdrAlgo.h"

namespace qcamera {

// Direct R11/R11s MMS gyro core; no HAL1 objects or EIS2 commands. A core
// failure is delivered as an un-stabilised pass-through frame, not an ERROR.
class QCamera3MmsVstab {
public:
    using Image = QCamera3HdrAlgo::Image;
    struct Vertex { float x, y, z; };
    struct Configuration {
        const char *profilePath;
        uint32_t inputWidth, inputHeight;
        uint32_t outputWidth, outputHeight;
        // Maximum retained image count (1..8) and BOOTTIME wait after enqueue.
        uint32_t maxPending;
        int64_t maxWaitNs;
    };
    struct Frame {
        uint32_t frameNumber;
        int64_t timestampNs, exposureNs;
        float zoom;
    };
    struct Result {
        Frame frame;
        int32_t coreStatus = 0;
        // 0 = the core really warped this frame; otherwise the negative errno of
        // the pass-through fallback (-ETIMEDOUT/-EIO/-EPROTO/-EUCLEAN).
        int32_t fallbackReason = 0;
        uint32_t sessionFrameIndex = 0;
        // frame.timestampNs - newest accepted gyro timestamp (CLOCK_BOOTTIME), else INT64_MIN.
        int64_t gyroDeltaNs = INT64_MIN;
        int32_t addGyroFailures = 0;
        bool coreDegraded = false;
    };

    QCamera3MmsVstab();
    ~QCamera3MmsVstab();
    static bool isAvailable();
    // Profile frame and gyro timers are both CLOCK_BOOTTIME (7).
    int32_t configure(const Configuration &config);
    int32_t addGyro(int64_t timestampNs, const float angularVelocity[3]);
    // Copies NV21/NV12; source may be released on success.
    int32_t enqueue(const Image &source, const Frame &frame, int64_t nowNs);
    // -EAGAIN retains the head; success or a core error consumes it.
    // -EINVAL/-ENODEV/-ENOENT do not consume; destination changes only on success.
    int32_t processNext(const Image &destination, int64_t nowNs, Result &result);
    // Releases a failed/timed-out head frame.
    int32_t discard(uint32_t frameNumber);
    // Cancels and releases all retained frames; reconfigure to restart.
    void flush();
    size_t pending() const;
    int64_t gyroPeriodNs() const;
    bool coreDegraded() const;
    int32_t lastCoreStatus() const;
    int32_t addGyroFailures() const;

    // CPU reference renderer; same mesh/UV topology as the GPU path.
    static int32_t render(const Image &source, const Image &destination,
            const Vertex *vertices, uint32_t columns, uint32_t rows,
            const std::atomic<bool> *cancelled = nullptr);

    // Un-stabilised full-frame bilinear scale source -> destination; used for EIS
    // OFF and as the fallback when the core fails. Returns 0 or a negative errno.
    static int32_t renderPassthrough(const Image &source, const Image &destination,
            const std::atomic<bool> *cancelled = nullptr);

private:
    struct Impl;
    Impl *mImpl;
    QCamera3MmsVstab(const QCamera3MmsVstab &) = delete;
    QCamera3MmsVstab &operator=(const QCamera3MmsVstab &) = delete;
};

} // namespace qcamera
#endif
