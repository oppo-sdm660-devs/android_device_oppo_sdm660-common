/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_DUAL_CAPTURE_H
#define QCAMERA3_DUAL_CAPTURE_H

#include "QCamera3DualBokehAlgo.h"
#include <memory>
#include <mutex>

namespace qcamera {

// Owns image copies only; the caller owns DMA/cache and JPEG handoff.
class QCamera3DualCapture {
public:
    using Image = QCamera3DualBokehAlgo::Image;
    using Calibration = QCamera3DualBokehAlgo::UnitCalibration;
    struct Rect { int32_t left, top, width, height; };
    struct Sensor {
        uint32_t cameraHandle;
        uint32_t referenceWidth, referenceHeight;
        // Scaler region in actual sensor-output coordinates.
        Rect referenceCrop;
        Rect activeArrayCrop;
        uint32_t imageWidth, imageHeight;
        Rect streamCrop, streamRoiMap;
        bool requireStreamCrop;
    };
    struct Config {
        Sensor sensors[2];
        float wideMaxFov;
        int64_t maxTimestampDeltaNs;
        int64_t timeoutNs;
    };
    struct Metadata {
        int64_t timestampNs, exposureNs, frameDurationNs;
        int32_t sensitivity;
        Rect crop;
        int32_t aeState, afState, awbState;
        Rect streamCrop, streamRoiMap;
    };
    struct Request {
        uint32_t frameIds[2];
        // AF point in wide sensor-output coordinates (see referenceCrop).
        int32_t focusX, focusY, blurLevel;
    };

    QCamera3DualCapture() = default;
    ~QCamera3DualCapture();
    static bool validConfig(const Config &config);
    static bool mapFocus(const Rect &crop, uint32_t width, uint32_t height,
            int32_t x, int32_t y, int32_t &imageX, int32_t &imageY);
    int32_t begin(const Config &, const Request &, const Calibration &, uint64_t &token);
    uint64_t tokenFor(uint32_t cameraHandle, uint32_t frameNumber) const;
    int32_t submitMetadata(uint64_t token, uint32_t cameraHandle, uint32_t frameNumber,
            const Metadata &metadata);
    // Copies visible rows immediately; source slots can then return to CPP.
    int32_t submitImage(uint64_t token, uint32_t cameraHandle, uint32_t frameNumber,
            const Image &image);
    bool isReady(uint64_t token) const;
    bool hasActiveCapture() const;
    // -EAGAIN until complete; output and wide identity untouched on failure.
    int32_t process(uint64_t token, const Image &destination);
    bool cancel(uint64_t token);
    void flush();

private:
    struct Group;
    mutable std::mutex mMutex;
    std::shared_ptr<Group> mGroup;
    uint64_t mNextToken = 0;
};
} // namespace qcamera
#endif
