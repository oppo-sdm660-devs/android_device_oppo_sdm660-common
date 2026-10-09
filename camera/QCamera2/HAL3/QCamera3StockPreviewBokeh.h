/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_STOCK_PREVIEW_BOKEH_H
#define QCAMERA3_STOCK_PREVIEW_BOKEH_H

#include "QCamera3DualBokehAlgo.h"
#include "QCamera3PreviewBokehGeometry.h"
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
        int64_t timestampsNs[2]; // Same-native sensor metadata exposure starts, in BOOTTIME.
        float mainLensShiftUm = 0;
        // Same-native MAIN AF result, in sensor coordinates (input pixels without Frame).
        QCamera3PreviewBokehGeometry::Rect afRegion = {};
        uint32_t faceFrameNumber = 0, faceCount = 0;
        // 0 missing, 1 exact nonempty source, 2 exact empty, 3 only older FD available.
        int32_t faceStatus = 0;
        QCamera3PreviewBokehGeometry::Rect selectedFace = {};
    };
    struct Focus { int32_t x, y, blurLevel; bool sensorCoordinates = false; };
    struct Result {
        Pair source = {};
        int32_t refocusRoi[4] = {};
        int32_t faceRoi[4] = {};
        int32_t coreStatus = 0, effectStatus = 0;
        int64_t geometryPrepareNs = 0, geometryProjectNs = 0, coreProcessNs = 0;
        bool applied = false;
    };

    QCamera3StockPreviewBokeh();
    ~QCamera3StockPreviewBokeh();
    static bool isAvailable();
    int32_t configure(const Config &, const Calibration &);
    // NV21 only. No driver buffers are retained. The proprietary call cannot be
    // interrupted; cancellation discards its output before destination commit.
    // Passthrough output remains -ENODATA, not applied bokeh.
    int32_t process(const Pair &, const Image &wide, const Image &tele,
            const Image &destination, const Focus &, Result &,
            const std::atomic<bool> *cancelled = nullptr,
            const QCamera3PreviewBokehGeometry::Frame *geometry = nullptr);
    void reset();

private:
    struct Impl;
    std::mutex mMutex;
    std::unique_ptr<Impl> mImpl;
};

} // namespace qcamera
#endif
