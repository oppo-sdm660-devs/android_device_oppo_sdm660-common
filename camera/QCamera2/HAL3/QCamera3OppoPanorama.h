/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_OPPO_PANORAMA_H
#define QCAMERA3_OPPO_PANORAMA_H

#include <stddef.h>
#include <stdint.h>

namespace qcamera {

/*
 * OSS reimplementation of the stock DocPanorama pipeline (task-135/139).
 *
 * Scope: pure C++ backend logic (no Android / no vendor blob / no stock HAL
 * module). The stock engine is FotoNation LibFotoStitch (LFS) linked inside
 * camera.sdm660.so; its internals are not recoverable, so this backend is a
 * behaviour-equivalent approximation (gyro prior + image shift estimation,
 * feather blending, exposure matching, bbox crop). It is NOT bit/pixel exact.
 *
 * Frame/gyro plumbing stays in HAL3 HWI/Channel (integration owned by hal-dev);
 * this class only consumes NV21 frames + accumulated gyro angles and produces
 * a stitched NV21 canvas. JPEG encoding is intentionally left to the caller
 * (PostProc/JPEG encoder hook) so the backend stays host-testable.
 */
class QCamera3OppoPanorama {
public:
    static constexpr int32_t DIR_AUTO = -1; // direction unknown: image-only guess

    struct Config {
        uint32_t previewWidth = 0;
        uint32_t previewHeight = 0;
        // Final output size. 0 => derived from the stock aspect table below.
        uint32_t outputWidth = 0;
        uint32_t outputHeight = 0;
        // Canvas (working image) size. 0 => previewWidth*3 x previewHeight.
        uint32_t canvasWidth = 0;
        uint32_t canvasHeight = 0;
        int32_t orientationDeg = 0; // device orientation: 0/90/180/270
        int32_t directionDeg = DIR_AUTO; // pan direction: 0/90/180/270 or AUTO
        bool crop = false;          // stock isDocPanoramaCropMode
        uint32_t maxFrames = 12;    // hard cap; stock had no explicit cap (D)
        uint32_t minFrames = 2;     // fewer frames => E_NOFRAME on stitch
        uint32_t featherPx = 24;    // linear feather width in canvas px
        // Gyro prior scale: px per degree. 0 => disabled (image-only).
        float pxPerDeg = 0.0f;
        // Search window for image-based shift refinement, canvas px.
        int32_t searchRadiusPx = 64;
        // Memory guard for canvas + weights + frame copy.
        size_t maxCanvasBytes = 96u * 1024u * 1024u;
        float maxExposureGain = 1.25f; // clamp for overlap gain matching
    };

    enum Status : int32_t {
        OK = 0,
        E_STATE = -1,      // call out of order
        E_PARAM = -2,      // bad config/frame
        E_NOMEM = -3,      // memory guard
        E_LIMIT = -4,      // maxFrames reached
        E_NOFRAME = -5,    // stitch with too few accepted frames
        E_CANCELED = -6,   // cancel() called
        E_OVERFLOW = -7,   // frame does not fit canvas
        E_UNSUPPORTED = -8 // invalid direction/format
    };

    enum class State {
        Idle,
        Inited,
        Capturing,
        Finetuned,
        Stitched,
        Canceled,
        Failed,
    };

    // NV21 (YUV420SP) image description. For Frame only data/length are
    // validated; width/height/stride must match the configured preview.
    struct Image {
        uint8_t *data = nullptr;
        size_t length = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t yStride = 0;   // >= width
        uint32_t uvStride = 0;  // >= width
        size_t yOffset = 0;
        size_t uvOffset = 0;    // yOffset + yStride*height
        int32_t orientationDeg = 0;
    };

    struct Frame {
        const uint8_t *data = nullptr;
        size_t length = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t yStride = 0;
        uint32_t uvStride = 0;
        int64_t timestampUs = 0;
    };

    // Accumulated angles from the HAL gyro path (task-139: degrees, rad/s input
    // integrated over us time base). Both angles may be 0 if gyro is absent.
    struct GyroSample {
        float angleXDeg = 0.0f;
        float angleYDeg = 0.0f;
        int64_t timestampUs = 0;
        bool valid = false;
    };

    struct Offset {
        int32_t x = 0; // current cumulative placement of the last frame
        int32_t y = 0;
    };

    struct Stats {
        uint32_t frames = 0;
        uint32_t rejected = 0;
        int32_t lastStatus = OK;
        size_t peakBytes = 0;
        float lastGain = 1.0f;
        int32_t shiftPx = 0; // image-only estimate of the last frame
    };

    QCamera3OppoPanorama();
    ~QCamera3OppoPanorama();
    QCamera3OppoPanorama(const QCamera3OppoPanorama &) = delete;
    QCamera3OppoPanorama &operator=(const QCamera3OppoPanorama &) = delete;

    // create/init/reset/cancel/deinit
    int32_t init(const Config &config);
    int32_t reinit(const Config &config);
    void deinit();
    void cancel();

    // Per-frame input. gyro may be nullptr (image-only fallback).
    int32_t addFrame(const Frame &frame, const GyroSample *gyro);
    int32_t addGyro(const GyroSample &gyro); // optional between frames

    // Finalize.
    int32_t finetune();
    // Required output buffer size in bytes for the final NV21 image.
    size_t outputSize() const;
    // Stitch into caller-provided NV21 buffer (Y then UV, stride = out width).
    // orientationDeg is applied (90/180/270 rotate). On error out is untouched.
    int32_t stitch(Image *out);

    // Introspection.
    State state() const { return state_; }
    const Config &config() const { return config_; }
    Offset currentOffset() const { return offset_; }
    const Stats &stats() const { return stats_; }
    uint32_t canvasWidth() const { return canvasW_; }
    uint32_t canvasHeight() const { return canvasH_; }
    uint32_t outputWidth() const { return outW_; }
    uint32_t outputHeight() const { return outH_; }

    // Stock aspect table (task-139, read from stock ctor call site):
    // 4:3 -> 320x240, 16:9 -> 640x360, ~0x13 ratio -> 646x306,
    // ~0x0d ratio -> 650x300. Returns false for unknown aspect.
    static bool stockAspectSize(uint32_t width, uint32_t height,
            uint32_t *outWidth, uint32_t *outHeight);
    // Stock direction code: 0->0x12, 90->0x11, 180->0x22, 270->0x21.
    static int32_t stockDirectionCode(int32_t directionDeg);

    static size_t nv21Size(uint32_t width, uint32_t height, uint32_t yStride,
            uint32_t uvStride);
    static bool validFrame(const Frame &frame);

private:
    struct Impl;
    Impl *impl_;
    int32_t fail(int32_t status, const char *where);
    void estimateShift(const Frame &frame, bool gyroValid,
            int32_t *shiftX, int32_t *shiftY);
    static void applySign(int sign, int32_t *shiftX, int32_t *shiftY);
    Config config_;
    State state_;
    Stats stats_;
    Offset offset_;
    uint32_t canvasW_;
    uint32_t canvasH_;
    uint32_t outW_;
    uint32_t outH_;
};

} // namespace qcamera
#endif
