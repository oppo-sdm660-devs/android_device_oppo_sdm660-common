/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_MMS_VSTAB_H
#define QCAMERA3_MMS_VSTAB_H

#include "QCamera3HdrAlgo.h"

namespace qcamera {

// Direct R11/R11s MMS gyro core; no HAL1 objects or EIS2 commands. This
// component does not advertise a capability: the channel must first establish
// the calibrated sensor, gyro and acquisition mode represented by its profile.
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
        int32_t coreStatus;
    };

    QCamera3MmsVstab();
    ~QCamera3MmsVstab();
    static bool isAvailable();
    // Accepts only normalized profiles whose frame and gyro timers are both
    // CLOCK_BOOTTIME (7). Geometry/grid counts are checked against the file and
    // the engine config. Original authentication remains in normal creation.
    int32_t configure(const Configuration &config);
    int32_t addGyro(int64_t timestampNs, const float angularVelocity[3]);
    // Copies active NV21/NV12 rows; the caller may release the source after
    // success. Chroma ordering is preserved, never converted implicitly.
    int32_t enqueue(const Image &source, const Frame &frame, int64_t nowNs);
    // FIFO, synchronized with gyro submission. -EAGAIN retains the head;
    // success or an algorithm/timeout/render error consumes it and identifies
    // it in result. -EINVAL/-ENODEV/-ENOENT do not consume a frame. Destination
    // changes only on success. Call with the head frame's retained HAL buffer.
    int32_t processNext(const Image &destination, int64_t nowNs, Result &result);
    // Release an externally failed/timed-out head without keeping its image or
    // accidentally applying its mesh to the next framework buffer.
    int32_t discard(uint32_t frameNumber);
    // Cancels commit before waiting for the synchronous core call; releases
    // every retained image and destroys gyro history. Reconfigure to restart.
    void flush();
    size_t pending() const;
    int64_t gyroPeriodNs() const;

    // Independently implemented plane renderer. Forward normalized GL mesh,
    // regular source UVs, stock alternating-row triangle-strip topology. No
    // inverse-coordinate assumption or GraphicBuffer object ABI is involved.
    static int32_t render(const Image &source, const Image &destination,
            const Vertex *vertices, uint32_t columns, uint32_t rows,
            const std::atomic<bool> *cancelled = nullptr);

private:
    struct Impl;
    Impl *mImpl;
    QCamera3MmsVstab(const QCamera3MmsVstab &) = delete;
    QCamera3MmsVstab &operator=(const QCamera3MmsVstab &) = delete;
};

} // namespace qcamera
#endif
