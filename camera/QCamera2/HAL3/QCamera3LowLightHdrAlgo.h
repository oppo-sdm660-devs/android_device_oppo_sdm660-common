/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_LOW_LIGHT_HDR_ALGO_H
#define QCAMERA3_LOW_LIGHT_HDR_ALGO_H

#include "QCamera3HdrAlgo.h"

namespace qcamera {

class QCamera3LowLightHdrAlgo {
public:
    using Image = QCamera3HdrAlgo::Image;

    static bool isAvailable();
    // Stock order: four normal images then one short exposure; cameraType is
    // rear=0/front=1. Destination is unchanged on error or cancellation.
    static int32_t process(const Image images[5], const Image &destination,
            int32_t iso, float drcGain, int32_t cameraType,
            const std::atomic<bool> *cancelled = nullptr);
};

} // namespace qcamera
#endif
