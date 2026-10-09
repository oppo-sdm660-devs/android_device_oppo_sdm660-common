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
    enum { MAX_FRAMES = 5, MAX_FACES = 10 };
    struct Face {
        int32_t left, top, width, height;
        uint8_t confidence;
    };
    struct FaceSet {
        uint32_t count = 0;
        Face faces[MAX_FACES] = {};
    };
    struct Options {
        // Metadata, timestamp and geometry stay anchored to this frame.
        uint32_t referenceIndex = 0;
        bool enhanceDetails = false;
        // Requires an exactly 2x destination.
        bool superResolution = false;
        bool fuseFaces = false;
        uint8_t beautyStrength = 0;
        uint8_t portraitLightStrength = 0;
        const FaceSet *faces = nullptr; // count entries, image pixel coordinates
    };
    struct Result {
        uint32_t referenceIndex = 0;
        uint32_t bestFrameIndex = 0; // quality suggestion only
        uint32_t acceptedFrames = 0;
        uint32_t replacedFaces = 0;
        bool reconstructed = false;
    };
    static bool isAvailable() { return true; }
    static int32_t process(const Image *images, uint32_t count,
            const Image &destination, const Options &options, Result *result,
            const std::atomic<bool> *cancelled = nullptr);
};

} // namespace qcamera
#endif
