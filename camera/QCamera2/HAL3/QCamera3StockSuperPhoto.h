/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_STOCK_SUPERPHOTO_H
#define QCAMERA3_STOCK_SUPERPHOTO_H
#include "QCamera3HdrAlgo.h"
namespace qcamera {
// Direct stock VDSuperPhoto core with stock tuning.
class QCamera3StockSuperPhoto {
public:
    using Image = QCamera3HdrAlgo::Image;
    enum { ZOOM_FRAME_COUNT = 4, HIGH_RESOLUTION_FRAME_COUNT = 5 };
    struct Source {
        uint32_t frameNumber = 0;
        int64_t timestampNs = 0;
        int64_t exposureTimeNs = 0;
        int32_t sensitivity = 0;
    };
    struct ZoomOptions {
        // Only the private burst may override Camera2 crop; a pre-cropped input
        // would be cropped a second time by VDSuperPhoto.
        bool fullFieldOfView = false;
    };
    struct ZoomResult {
        bool algorithmApplied = false;
        float zoomRatio = 0;
        uint32_t submittedFrames = 0;
        uint32_t referenceIndex = 0;
        Source sources[ZOOM_FRAME_COUNT] = {};
    };
    struct HighResolutionOptions {
        bool fullFieldOfView = false;
    };
    struct HighResolutionResult {
        bool algorithmApplied = false;
        uint32_t submittedFrames = 0;
        uint32_t referenceIndex = 0;
        Source sources[HIGH_RESOLUTION_FRAME_COUNT] = {};
    };
    static bool isAvailable();
    static bool is2xAvailable();
    static bool isHighResolutionAvailable();
    // Existing explicit SUPER is equal-size, 1x fusion; it is not the stock 2x entry.
    static int32_t process(const Image *images, uint32_t count, const Image &output,
            const char *sensorName, int32_t iso, bool frontCamera,
            const std::atomic<bool> *cancelled = nullptr);
    // Rear single-sensor, four current-capture equal-exposure full-FOV frames.
    // The engine owns alignment and 2x ROI reconstruction; output stays normal size.
    static int32_t process2x(const Image *images, const Source *sources, uint32_t count,
            const Image &output, const char *sensorName, const ZoomOptions &options,
            ZoomResult *result, const std::atomic<bool> *cancelled = nullptr);
    // Professional high-resolution: five full-FOV frames, zoom=1, twice the
    // input width/height. Large output geometry is independent of input stride.
    static int32_t processHighResolution(const Image *images, const Source *sources,
            uint32_t count, const Image &output, const char *sensorName,
            const HighResolutionOptions &options, HighResolutionResult *result,
            const std::atomic<bool> *cancelled = nullptr);
};
}
#endif
