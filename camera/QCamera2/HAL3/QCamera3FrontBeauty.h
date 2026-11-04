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
// The face-hint record layout and per-feature semantics are not established, so
// this adapter passes no face hint and the HWI keeps it behind
// persist.vendor.camera.oppo.front_beauty.enable until it is device-verified.
class QCamera3FrontBeauty {
public:
    using Image = QCamera3HdrAlgo::Image;

    // dlopen + resolve the evidenced exports; false means "do not announce".
    static bool isAvailable();
    // NV21 only. strength is 0..100 and is passed to every evidenced feature id.
    // Owns a YV12 scratch image: the destination is unchanged on error/cancel.
    static int32_t process(const Image &image, int32_t strength,
            const std::atomic<bool> *cancelled = nullptr);
};

} // namespace qcamera
#endif
