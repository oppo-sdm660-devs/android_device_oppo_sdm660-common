/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_POSTPROCESS_ASSETS_H
#define QCAMERA3_POSTPROCESS_ASSETS_H

#include "QCamera3HdrAlgo.h"

#include <stddef.h>
#include <stdint.h>

namespace qcamera {

// Graphical watermark / sticker / mirror engine for NV21 frames.
//
// Stock behavior replaced: the HAL1 parameters KEY_WATERMARK_ENABLE
// ("oppo.camera.watermark.enable"), KEY_STICKER_MODE, the app supplied
// watermark YUV path (libcamera_core_hwi.so diag strings "watermark yuvpath =
// %s", "watermark : pic w=%d h=%d stride=%d scanline=%d"), processWatermarkFill()
// on the preview path and offlineWatermarkFill() on the still path. This class
// composites caller or vendor-pack assets itself; no HAL1 object, no EGL/GLES
// engine and no stock C++ ABI is involved. The app-facing ASCII watermark and
// the <=32x32 inline ARGB overlay stay in org.lineageos.camera.postprocess
// (extensions/POSTPROCESS_CONTROLS.md); this stage is the native pixel engine
// behind them and behind the new watermarkAsset/stickerAsset keys.
//
// Composition rules (exact, deterministic, integer only):
//   alpha     : a = (assetAlpha * opacity + 127) / 255        (0..255)
//   luma      : d = (s * a + d * (255 - a) + 127) / 255
//   chroma    : per 2x2 NV21 block, a_block = (a00 + a01 + a10 + a11 + 2) / 4,
//               V/U blended with the same 255 divisor; the block's source chroma
//               is converted from the asset's top-left pixel of the block with
//               BT.601 limited range integer coefficients.
//   sampling  : bilinear, Q15 weights, at most 4 source texels, edge clamped.
//               A 1:1 placement samples texel centres exactly (bit-exact copy).
//   order     : mirror the frame first, then watermark, then sticker; asset
//               rectangles are always in the final (post-mirror) output frame,
//               matching POSTPROCESS_CONTROLS.md ("placed after mirror").
//
// Two callers, one pixel engine:
//   process()          preview/video NV21 frame in the frame's own orientation.
//   processJpegEdit()  the unrotated NV21 edit surface of the JPEG pipeline
//                      (before the encoder and before EXIF orientation). The
//                      rectangles are supplied in the *viewed* orientation and
//                      are mapped back onto the unrotated surface, so preview
//                      and photo agree for the same request. Composition never
//                      runs on compressed JPEG bytes: a compressed JPEG has no
//                      editable pixels. The encoded blob is edited by
//                      QCamera3ExifExtras only (metadata, not pixels).
//
// Bypass: `enable == 0`, or no mirror and no visible asset, leaves the
// destination byte-identical (it is not even copied). Errors, malformed assets,
// bounds violations and cancellation leave the destination untouched.
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

    // Normalized rectangle in the coordinate system of the frame that is
    // written. Must be finite, ordered and inside [0,1].
    struct Rect {
        float left = 0.0f;
        float top = 0.0f;
        float right = 0.0f;
        float bottom = 0.0f;
    };

    // Caller-owned asset. ARGB32 uses `stride` pixels per row; NV21 uses
    // `stride` bytes of luma per row with the UV plane at offset
    // width*height and the same stride. `length` must cover the whole asset.
    struct Asset {
        const void *pixels = nullptr;
        size_t length = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t stride = 0;
        int32_t format = 0;
    };

    struct Options {
        // Master switch (appMask bit 8 / EXTENSION_STRENGTH > 0). Zero is a
        // strict bypass.
        int32_t enable = 0;
        // Horizontal mirror of the written frame (mirrorPreview / mirror).
        int32_t mirror = 0;
        // Master decoration alpha 0..255, scales every asset alpha.
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
        int32_t applied = 0;   // 1 when at least one pixel or the mirror changed
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

    static const char *name();  // trace id "PostprocessAssets"
    static bool isAvailable();  // pure native engine, no stock core
    static bool supports(uint32_t width, uint32_t height);
    static bool validOptions(const Options &options);
    static bool validRect(const Rect &rect);
    static bool validAsset(const Asset &asset);
    // Pixel span of a normalized rectangle in an extent x extent frame:
    // [round(left*extent), round(right*extent)) with round = floor(v + 0.5).
    // Returns -EINVAL for a degenerate span.
    static int32_t resolveRect(const Rect &normalized, uint32_t extentX, uint32_t extentY,
            uint32_t &left, uint32_t &top, uint32_t &right, uint32_t &bottom);
    // Maps a rectangle expressed in the viewed (display) orientation back onto
    // the unrotated JPEG edit surface. `rotationDegrees` is the clockwise
    // rotation that the viewer applies for display (0/90/180/270), i.e. the
    // value written to JPEG_ORIENTATION.
    static int32_t mapRectForJpeg(const Rect &viewed, int32_t rotationDegrees, Rect &mapped);

    // Vendor pack support. installedRoot() is the only accepted root for
    // org.lineageos.camera.ar/postprocess asset file names:
    //   /vendor/etc/camera/postprocess
    // loadAsset() enforces it; loadAssetAt() is the same bounded loader with an
    // explicit root for fixtures/tooling. File names carry their own geometry:
    //   <id>_<W>x<H>.nv21 | <id>_<W>x<H>.argb
    // The file size must match the declared geometry exactly. Symlinks, "..",
    // paths outside the root, non-regular files, empty/oversized files, bad
    // geometry and unknown extensions fail closed.
    static const char *installedRoot();
    static int32_t parseAssetName(const char *name, uint32_t &width, uint32_t &height,
            int32_t &format);
    static int32_t loadAsset(const char *source, uint8_t *storage, size_t capacity,
            size_t &length, Asset &out);
    static int32_t loadAssetAt(const char *root, const char *source, uint8_t *storage,
            size_t capacity, size_t &length, Asset &out);

    // Preview/video path: NV21 source -> NV21 destination, one bounded scratch
    // frame (documented single allocation, <= MAX_SCRATCH_BYTES). In-place
    // (source == destination) is allowed; the result is committed atomically.
    int32_t process(const Image &source, const Image &destination, const Options &options,
            Result &result, const std::atomic<bool> *cancelled = nullptr);
    // JPEG edit path: identical pixel engine, rectangles first mapped from the
    // viewed orientation onto the unrotated edit surface.
    int32_t processJpegEdit(const Image &source, const Image &destination,
            int32_t rotationDegrees, const Options &options, Result &result,
            const std::atomic<bool> *cancelled = nullptr);

    // No retained state exists; provided for the common stage shape.
    void reset() {}
};

} // namespace qcamera
#endif
