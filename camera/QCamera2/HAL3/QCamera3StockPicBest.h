/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_STOCK_PIC_BEST_H
#define QCAMERA3_STOCK_PIC_BEST_H

#include "QCamera3HdrAlgo.h"

namespace qcamera {

class QCamera3StockPicBest {
public:
    using Image = QCamera3HdrAlgo::Image;
    static constexpr uint32_t kMaxFrames = 10, kMaxFaces = 10;
    struct Face {
        int32_t id, left, top, width, height;
        float rollDegrees;
    };
    struct Frame {
        // Interleaved UV (NV12), with face rectangles in visible source pixels.
        Image image;
        uint32_t frameNumber;
        int64_t timestampNs;
        uint32_t faceCount;
        Face faces[kMaxFaces];
    };
    struct Selection {
        uint32_t index = 0, frameNumber = 0;
        int64_t timestampNs = 0;
        uint32_t matchedFaces = 0;
        int32_t faceIds[kMaxFaces] = {};
        int32_t eyeScores[kMaxFrames][kMaxFaces] = {};
        bool fusionApplied = false;
    };

    // Pixel-stage ABI only: no HAL1 parameters, callbacks, or capture queues.
    static bool isAvailable(const char *corePath = "/vendor/lib/libcamera_core_hwi.so");
    static bool isFusionAvailable(const char *corePath = "/vendor/lib/libcamera_core_hwi.so");
    // Eye-open ranking from the stock detector. Selects a whole source frame;
    // it does not claim stock's per-face replacement/fusion. No image is modified.
    static int32_t select(const Frame *, uint32_t count, Selection &,
            const std::atomic<bool> *cancelled = nullptr,
            const char *corePath = "/vendor/lib/libcamera_core_hwi.so");
    // Stock per-face replacement writes a private copy of reference frame zero.
    // The source metadata remains that reference's; inputs stay immutable.
    static int32_t fuse(const Frame *, uint32_t count, const Image &output, Selection &,
            const std::atomic<bool> *cancelled = nullptr,
            const char *corePath = "/vendor/lib/libcamera_core_hwi.so");
    static int32_t fuseNv21(const Frame *, uint32_t count, const Image &output, Selection &,
            const std::atomic<bool> *cancelled = nullptr,
            const char *corePath = "/vendor/lib/libcamera_core_hwi.so");
private:
    static int32_t process(const Frame *, uint32_t count, Selection &, const Image *output,
            const std::atomic<bool> *, const char *corePath, bool nv21 = false);
};

} // namespace qcamera
#endif
