/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_STILL_ALGO_H
#define QCAMERA3_STILL_ALGO_H

#include "QCamera3HdrAlgo.h"

namespace qcamera {

// Source implementation for equal-exposure NV21 bursts (no stock StillMore ABI).
// Inputs stay caller-owned; the destination is committed on success.
class QCamera3StillAlgo {
public:
    using Image = QCamera3HdrAlgo::Image;
    enum { MAX_FRAMES = 5 };
    struct Options {
        // Metadata, timestamp and geometry stay anchored to this frame.
        uint32_t referenceIndex = 0;
    };
    struct Result {
        uint32_t referenceIndex = 0;
        uint32_t acceptedFrames = 0;
        bool pixelsChanged = false;
    };
    static bool isAvailable() { return true; }
    static int32_t process(const Image *images, uint32_t count,
            const Image &destination, const Options &options, Result *result,
            const std::atomic<bool> *cancelled = nullptr);
};

} // namespace qcamera
#endif
