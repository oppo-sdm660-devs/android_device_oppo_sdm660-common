/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_HDR_ALGO_H
#define QCAMERA3_HDR_ALGO_H

#include <atomic>
#include <stddef.h>
#include <stdint.h>

namespace qcamera {

// HAL3-owned image layout. Offsets include each plane's actual data offset;
// allocation lengths and scanlines are never inferred from another stream.
class QCamera3HdrAlgo {
public:
    struct Image {
        uint8_t *data;
        size_t length;
        uint32_t width, height;
        size_t yOffset, uvOffset;
        uint32_t yStride, uvStride;
        uint32_t yScanline, uvScanline;
    };

    static bool isAvailable();
    static bool validImage(const Image &image);
    // Input order is base, underexposed, overexposed. The core modifies scratch
    // images only. Destination remains unchanged on any error/cancellation.
    static int32_t process(const Image images[3], const Image &destination,
            bool frontCamera, const std::atomic<bool> *cancelled = nullptr);
};

} // namespace qcamera
#endif
