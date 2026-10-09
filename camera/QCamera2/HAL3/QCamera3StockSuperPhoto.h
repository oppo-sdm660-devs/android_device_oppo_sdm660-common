/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_STOCK_SUPERPHOTO_H
#define QCAMERA3_STOCK_SUPERPHOTO_H
#include "QCamera3HdrAlgo.h"
namespace qcamera {
// Direct stock VDSuperPhoto core with stock tuning; output is equal-size, not 2x.
class QCamera3StockSuperPhoto {
public:
    using Image = QCamera3HdrAlgo::Image;
    static bool isAvailable();
    static int32_t process(const Image *images, uint32_t count, const Image &output,
            const char *sensorName, int32_t iso, bool frontCamera,
            const std::atomic<bool> *cancelled = nullptr);
};
}
#endif
