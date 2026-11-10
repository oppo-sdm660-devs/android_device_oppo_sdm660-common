/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_STOCK_FILTER_H
#define QCAMERA3_STOCK_FILTER_H

#include "QCamera3HdrAlgo.h"

namespace qcamera {

class QCamera3StockFilter {
public:
    using Image = QCamera3HdrAlgo::Image;
    static bool isAvailable();
    static bool supportsType(int32_t type);
    // Only call for an explicitly enabled filter; type 0 is Ethereal, not off.
    // flipMode is the stock color-mask orientation (0/1/2), not image rotation.
    // NV21 destination and padding remain unchanged on failure/cancellation.
    static int32_t process(const Image &image, int32_t type, int32_t flipMode = 0,
            const std::atomic<bool> *cancelled = nullptr);
    static int32_t processRgba(uint8_t *rgba, size_t length, uint32_t width,
            uint32_t height, uint32_t stride, int32_t type, int32_t flipMode = 0,
            const std::atomic<bool> *cancelled = nullptr);
};

} // namespace qcamera
#endif
