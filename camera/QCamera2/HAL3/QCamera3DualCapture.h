/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_DUAL_CAPTURE_H
#define QCAMERA3_DUAL_CAPTURE_H

#include "QCamera3DualBokehAlgo.h"
#include <memory>
#include <mutex>

namespace qcamera {

// One bounded, token-scoped still pair. This owns image copies, never HAL1
// objects or camera3 buffers. The caller owns DMA/cache and JPEG handoff.
class QCamera3DualCapture {
public:
    using Image = QCamera3DualBokehAlgo::Image;
    using Calibration = QCamera3DualBokehAlgo::UnitCalibration;
    struct Rect { int32_t left, top, width, height; };
    struct Sensor {
        uint32_t cameraHandle;
        uint32_t referenceWidth, referenceHeight;
        // Requested scaler region in actual sensor-output coordinates. The
        // per-frame CPP ROI below identifies the optical region of its image.
        Rect referenceCrop;
        Rect activeArrayCrop; // Same optical region in camera2 active-array coordinates.
        uint32_t imageWidth, imageHeight; // Zero permits bounded test/standalone input sizes.
        Rect streamCrop, streamRoiMap; // Full visible image, corresponding sensor ROI.
        bool requireStreamCrop;
    };
    struct Config {
        Sensor sensors[2]; // wide, tele
        float wideMaxFov;
        int64_t maxTimestampDeltaNs;
        int64_t timeoutNs;
    };
    struct Metadata {
        int64_t timestampNs, exposureNs, frameDurationNs;
        int32_t sensitivity;
        Rect crop; // Actual sensor-output crop for this frame.
        int32_t aeState, afState, awbState;
        Rect streamCrop, streamRoiMap;
    };
    struct Request {
        uint32_t frameIds[2];
        // AF point in the wide sensor-output coordinates used by referenceCrop.
        // The caller maps standard active-array AF regions before beginning.
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
    // Returns -EAGAIN until complete. Output is untouched on cancellation or any
    // failed core call. It has wide geometry and keeps wide timestamp/identity.
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
