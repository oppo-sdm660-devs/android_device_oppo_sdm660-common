/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_DUAL_BOKEH_ALGO_H
#define QCAMERA3_DUAL_BOKEH_ALGO_H

#include "QCamera3HdrAlgo.h"

namespace qcamera {

// Standalone capture core; the caller establishes calibrated ordering and sync images.
class QCamera3DualBokehAlgo {
public:
    using Image = QCamera3HdrAlgo::Image;
    // ARC_DCIR blur units.
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
    struct CalibrationFiles {
        const char *moduleOtp;
        const char *calibratedOutput;
        const char *savedFuse;
        const char *currentFuse;
    };

    static bool isAvailable();
    static bool validCalibration(const void *calibration, size_t length);
    // Stock file-selection policy: prefer calibrationOutput unless a saved fuse differs,
    // else module OTP; the result is cleared on failure.
    static int32_t loadCalibration(UnitCalibration &result);
    static int32_t loadCalibration(const CalibrationFiles &files,
            UnitCalibration &result);
    // Destination is written only after full success; no async abort, cancellation waits.
    static int32_t process(const Image &wide, const Image &tele,
            const Image &destination, const Geometry &geometry, const Focus &focus,
            const void *calibration, size_t calibrationLength,
            const std::atomic<bool> *cancelled = nullptr);
};

} // namespace qcamera
#endif
