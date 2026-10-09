/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_LOW_LIGHT_HDR_ALGO_H
#define QCAMERA3_LOW_LIGHT_HDR_ALGO_H

#include "QCamera3HdrAlgo.h"

namespace qcamera {

class QCamera3LowLightHdrAlgo {
public:
    using Image = QCamera3HdrAlgo::Image;

    static bool isAvailable();
    // Stock capture supplies four normal images and a short-exposure image.
    // ISO and DRC must belong to the retained normal image; cameraType is
    // rear=0/front=1. All processing uses owned, writable scratch images.
    // Destination remains unchanged on an error or observed cancellation.
    static int32_t process(const Image images[5], const Image &destination,
            int32_t iso, float drcGain, int32_t cameraType,
            const std::atomic<bool> *cancelled = nullptr);
};

} // namespace qcamera
#endif
