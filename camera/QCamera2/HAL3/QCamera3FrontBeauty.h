/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_FRONT_BEAUTY_H
#define QCAMERA3_FRONT_BEAUTY_H

#include "QCamera3HdrAlgo.h"

namespace qcamera {

// R11/R11s front-face beauty core from libarcsoft_beautyshot.so (ARC_BS_IMAGE_*).
// Evidence: stock vendor/lib/libcamera_core_hwi.so resolves ARC_BS_IMAGE_Init /
// ARC_BS_IMAGE_UnInit / ARC_BS_IMAGE_SetFeatureLevel / ARC_BS_IMAGE_Process and
// calls the latter with the same image as
// source and destination, a face hint as the 4th argument and NULL as the 5th.
// Face hints are optional; the core performs its own detection.
class QCamera3FrontBeauty {
public:
    using Image = QCamera3HdrAlgo::Image;
    struct FeatureLevels {
        uint8_t levels[7] = {};
    };

    // Includes a real Init/UnInit round trip, without an external Venus model.
    static bool isAvailable();
    // Default front-camera profile from the stock core's six-level table.
    // Face-dependent brightening/shape parameters need their own measured input.
    static FeatureLevels levelsForStrength(int32_t strength);
    // NV21 only. strength is 0..100; zero leaves the frame untouched.
    // Owns a YV12 scratch image: the destination is unchanged on error/cancel.
    // -ENODATA distinguishes an unchanged output (e.g. no usable face).
    static int32_t process(const Image &image, int32_t strength,
            const std::atomic<bool> *cancelled = nullptr);
    static int32_t process(const Image &image, const FeatureLevels &features,
            const std::atomic<bool> *cancelled = nullptr);
};

} // namespace qcamera
#endif
