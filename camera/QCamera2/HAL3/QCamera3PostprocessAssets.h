/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_POSTPROCESS_ASSETS_H
#define QCAMERA3_POSTPROCESS_ASSETS_H

#include "QCamera3HdrAlgo.h"

#include <stddef.h>
#include <stdint.h>

namespace qcamera {

// Graphical watermark / sticker / mirror engine for NV21 frames: integer blend
// with a 255 divisor (chroma per 2x2 block), Q15 bilinear, mirror then assets;
// bypass and error paths leave the destination untouched.
class QCamera3PostprocessAssets {
public:
    using Image = QCamera3HdrAlgo::Image;

    enum {
        FORMAT_ARGB32 = 1,  // 4 bytes/pixel, straight alpha in bits 24..31
        FORMAT_NV21 = 2,    // Y plane + interleaved V/U, 4:2:0
        MAX_ASSET_SIDE = 512,
        MAX_ASSET_BYTES = 1 << 20,
        MAX_SOURCE_NAME = 256,   // watermarkAsset/stickerAsset byte[] payload
        MAX_PATH_BYTES = 512,    // root + '/' + source
        MAX_SCALE = 16,          // bounded kernel footprint ratio, per axis
        CANCEL_ROWS = 64,        // cancellation latency bound (contract)
        MAX_IMAGE_SIDE = 8192,
        MAX_IMAGE_PIXELS = 16777216,
        MAX_SCRATCH_BYTES = 32 << 20,
        JPEG_ROTATION_0 = 0,
        JPEG_ROTATION_90 = 90,
        JPEG_ROTATION_180 = 180,
        JPEG_ROTATION_270 = 270,
    };

    // Normalized rect in the written frame; finite, ordered and inside [0,1].
    struct Rect {
        float left = 0.0f;
        float top = 0.0f;
        float right = 0.0f;
        float bottom = 0.0f;
    };

    // Caller-owned asset: stride is pixels/row for ARGB32 and bytes/row for NV21,
    // with the UV plane at width*height; length must cover the whole asset.
    struct Asset {
        const void *pixels = nullptr;
        size_t length = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t stride = 0;
        int32_t format = 0;
    };

    struct Options {
        // Master switch; 0 is a strict bypass.
        int32_t enable = 0;
        int32_t mirror = 0;
        // 0..255, scales every asset alpha.
        int32_t opacity = 255;
        Rect watermarkRect = {};  // org.lineageos.camera.postprocess.watermarkLayout
        Rect stickerRect = {};    // org.lineageos.camera.algorithms.stickerRect
        const Asset *watermark = nullptr;
        const Asset *sticker = nullptr;
        uint32_t frameNumber = 0;
        static bool valid(const Options &o);
    };

    struct Result {
        int32_t status = 0;    // 0 pass; negative errno otherwise
        int32_t applied = 0;
        uint32_t frameNumber = 0;
        // detail[0] = watermark pixels written,
        // detail[1] = sticker pixels written,
        // detail[2] = frame mirror applied (0/1),
        // detail[3] = fully transparent asset samples skipped.
        int32_t detail[4] = {};
        uint32_t watermarkPixels = 0;
        uint32_t stickerPixels = 0;
        uint8_t mirrored = 0;
        uint8_t bypassed = 0;
    };

    static const char *name();
    static bool isAvailable();
    static bool supports(uint32_t width, uint32_t height);
    static bool validOptions(const Options &options);
    static bool validRect(const Rect &rect);
    static bool validAsset(const Asset &asset);
    // Pixel span: [round(left*extent), round(right*extent)), round = floor(v+0.5);
    // -EINVAL for a degenerate span.
    static int32_t resolveRect(const Rect &normalized, uint32_t extentX, uint32_t extentY,
            uint32_t &left, uint32_t &top, uint32_t &right, uint32_t &bottom);
    // Maps a viewed-orientation rectangle back onto the unrotated edit surface;
    // rotationDegrees is the clockwise display rotation written to JPEG_ORIENTATION.
    static int32_t mapRectForJpeg(const Rect &viewed, int32_t rotationDegrees, Rect &mapped);

    // Vendor pack assets under /vendor/etc/camera/postprocess, named
    // <id>_<W>x<H>.nv21|.argb; the file size must match the geometry. Symlinks, "..",
    // non-regular and malformed files fail closed.
    static const char *installedRoot();
    static int32_t parseAssetName(const char *name, uint32_t &width, uint32_t &height,
            int32_t &format);
    static int32_t loadAsset(const char *source, uint8_t *storage, size_t capacity,
            size_t &length, Asset &out);
    static int32_t loadAssetAt(const char *root, const char *source, uint8_t *storage,
            size_t capacity, size_t &length, Asset &out);

    // NV21 source -> NV21 destination with one bounded scratch frame; in-place is
    // allowed and the result is committed atomically.
    int32_t process(const Image &source, const Image &destination, const Options &options,
            Result &result, const std::atomic<bool> *cancelled = nullptr);
    // Identical engine; rectangles are mapped back onto the unrotated surface.
    int32_t processJpegEdit(const Image &source, const Image &destination,
            int32_t rotationDegrees, const Options &options, Result &result,
            const std::atomic<bool> *cancelled = nullptr);

    void reset() {}
};

} // namespace qcamera
#endif
