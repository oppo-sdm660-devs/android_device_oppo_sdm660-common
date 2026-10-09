/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_DUAL_BOKEH_ALGO_H
#define QCAMERA3_DUAL_BOKEH_ALGO_H

#include "QCamera3HdrAlgo.h"

namespace qcamera {

// Standalone capture core only. Callers must first establish calibrated sensor
// ordering, uncropped reference geometry and synchronized wide/tele images.
class QCamera3DualBokehAlgo {
public:
    using Image = QCamera3HdrAlgo::Image;
    // ARC_DCIR blur units, shared by request admission and the core adapter.
    static constexpr int32_t kMaxBlurLevel = 27;
    static constexpr int32_t kDefaultBlurLevel = 14;
    struct Geometry {
        uint32_t wideWidth, wideHeight, teleWidth, teleHeight;
        float wideMaxFov;
    };
    struct Focus {
        int32_t x, y, blurLevel;
    };
    struct UnitCalibration {
        enum Source { NONE, MODULE_OTP, CALIBRATED_OUTPUT } source = NONE;
        uint8_t data[512] = {};
    };
    // Internal file inputs, never camera request controls. Explicit paths allow
    // the filesystem decision to be tested without touching device persist.
    struct CalibrationFiles {
        const char *moduleOtp;
        const char *calibratedOutput;
        const char *savedFuse;
        const char *currentFuse;
    };

    static bool isAvailable();
    static bool validCalibration(const void *calibration, size_t length);
    // Reads installed unit data using the stock file-selection policy. Use
    // calibrationOutput when readable unless an available saved fuse differs
    // from the current fuse; otherwise use module OTP. On failure, clear the
    // result so a previous loaded record cannot remain eligible.
    static int32_t loadCalibration(UnitCalibration &result);
    static int32_t loadCalibration(const CalibrationFiles &files,
            UnitCalibration &result);
    // No borrowed source/calibration memory reaches the core. Destination is
    // committed only after the whole lifecycle succeeds. No async abort API
    // exists: cancellation waits for an in-progress stock call to return.
    static int32_t process(const Image &wide, const Image &tele,
            const Image &destination, const Geometry &geometry, const Focus &focus,
            const void *calibration, size_t calibrationLength,
            const std::atomic<bool> *cancelled = nullptr);
};

} // namespace qcamera
#endif
