/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_STOCK_BEAUTY_H
#define QCAMERA3_STOCK_BEAUTY_H

#include "QCamera3HdrAlgo.h"

namespace qcamera {

// R11/R11s HAL1 BackFaceBeauty's single-image ArcSoft core; only that caller's
// four feature IDs are exposed, with explicit levels.
class QCamera3StockBeauty {
public:
    using Image = QCamera3HdrAlgo::Image;
    struct Options {
        uint8_t soften = 0;
        uint8_t softenSharp = 0;
        uint8_t skinBright = 0;
        uint8_t skinStyle = 0;
    };

    static bool isAvailable();
    // NV21 only; owns a writable padded copy and commits active rows only on success.
    // The ArcSoft core does its own face detection; no HAL1 object crosses this ABI.
    static int32_t process(const Image &image, const Options &options,
            const std::atomic<bool> *cancelled = nullptr);
};

} // namespace qcamera
#endif
