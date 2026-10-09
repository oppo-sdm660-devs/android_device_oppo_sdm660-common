/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_STOCK_BEAUTY_H
#define QCAMERA3_STOCK_BEAUTY_H

#include "QCamera3HdrAlgo.h"

namespace qcamera {

// R11/R11s HAL1 BackFaceBeauty's single-image ArcSoft core. This adapter
// deliberately exposes only the four feature IDs used by that caller, with
// explicit levels rather than a guessed sensor/region tuning table.
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
    // NV21 only. Owns a writable padded copy for the complete synchronous
    // Init/SetFeatureLevel/Process/UnInit lifecycle; commits active rows only
    // on success. Cancellation never races UnInit against an executing core.
    // The ArcSoft library performs its own face detection. No opaque HAL1
    // object, face record, or crop/sensor metadata is passed through this ABI.
    static int32_t process(const Image &image, const Options &options,
            const std::atomic<bool> *cancelled = nullptr);
};

} // namespace qcamera
#endif
