/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_STOCK_PREVIEW_BOKEH_H
#define QCAMERA3_STOCK_PREVIEW_BOKEH_H

#include "QCamera3DualBokehAlgo.h"
#include <memory>
#include <mutex>

namespace qcamera {

class QCamera3StockPreviewBokeh {
public:
    using Image = QCamera3HdrAlgo::Image;
    using Calibration = QCamera3DualBokehAlgo::UnitCalibration;
    struct Config {
        uint32_t cameraHandles[2];
        uint32_t imageWidths[2], imageHeights[2];
        uint32_t activeWidths[2], activeHeights[2];
        int32_t degree;
        int64_t maxTimestampDeltaNs;
    };
    // Pairing and calibration ordering come from the verified HAL3 session.
    struct Pair {
        uint32_t cameraHandles[2], frameNumbers[2];
        int64_t timestampsNs[2];
    };
    struct Focus { int32_t x, y, blurLevel; };
    struct Result {
        Pair source = {};
        int32_t coreStatus = 0, effectStatus = 0;
        bool applied = false;
    };

    QCamera3StockPreviewBokeh();
    ~QCamera3StockPreviewBokeh();
    static bool isAvailable();
    int32_t configure(const Config &, const Calibration &);
    // NV21 only. No driver buffers are retained. The proprietary call cannot be
    // interrupted; cancellation discards its output before destination commit.
    // -ENODATA includes the core's initial passthrough frames, not applied bokeh.
    int32_t process(const Pair &, const Image &wide, const Image &tele,
            const Image &destination, const Focus &, Result &,
            const std::atomic<bool> *cancelled = nullptr);
    void reset();

private:
    struct Impl;
    std::mutex mMutex;
    std::unique_ptr<Impl> mImpl;
};

} // namespace qcamera
#endif
