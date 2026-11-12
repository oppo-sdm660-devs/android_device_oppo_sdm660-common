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
    enum class PortraitFraming : int32_t { Half = 0, Full = 1 };
    struct Rect { int32_t left, top, width, height; };
    struct Sensor {
        uint32_t cameraHandle;
        uint32_t referenceWidth, referenceHeight;
        // Scaler region in actual sensor-output coordinates.
        Rect referenceCrop;
        Rect activeArrayCrop;
        uint32_t imageWidth, imageHeight;
        // Actual captured frame dimensions; imageWidth/imageHeight stay the stock
        // ARC reference and are reached by centre-crop + upscale when needed.
        uint32_t sourceWidth, sourceHeight;
        // Daemon scaler metadata can still report the binned raw mode; map it to
        // source coordinates before ARC.
        uint32_t metadataWidth, metadataHeight;
        Rect streamCrop, streamRoiMap;
        bool requireStreamCrop;
    };
    struct Config {
        Sensor sensors[2];
        float wideMaxFov;
        // Half framing requires a narrower auxiliary FOV; Full keeps the wide FOV.
        float teleMaxFov = 0.0f;
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
        PortraitFraming framing = PortraitFraming::Half;
    };

    QCamera3DualCapture() = default;
    ~QCamera3DualCapture();
    static bool validConfig(const Config &config);
    static bool mapFocus(const Rect &crop, uint32_t width, uint32_t height,
            int32_t x, int32_t y, int32_t &imageX, int32_t &imageY);
    // Derived from the bound optical FOVs; does not change stereo ordering or calibration.
    static bool framingCrop(const Config &, PortraitFraming, uint32_t width,
            uint32_t height, Rect &crop);
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
    // CPU NV12 centre-crop + bilinear scale; used by submitImage/process and
    // host tests. crop=true applies the FOV ratio, otherwise aspect crop only.
    static bool resize(const Image &from, const Image &to, bool crop,
            float ratio = 1.0f);
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
