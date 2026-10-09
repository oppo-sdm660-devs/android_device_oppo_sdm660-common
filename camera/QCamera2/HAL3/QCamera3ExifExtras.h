/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_EXIF_EXTRAS_H
#define QCAMERA3_EXIF_EXTRAS_H

#include <atomic>
#include <stddef.h>
#include <stdint.h>

namespace qcamera {

// EXIF UserComment / ImageDescription writer for an encoded JPEG byte buffer,
// plus the pure professional-info result helpers the HAL advertises.
//
// Stock behavior replaced: the HAL1 parameter KEY_EXIF_COMMENTS
// ("exif-comments", logged as "exifcomments:%s strlen:%d") written through
// libexif by the stock postprocessor (exif_entry_new_mem / exif_data_save_data)
// and the stock EXIF debug payload. This class calls no libexif, links no
// Android framework class and never touches compressed image data: it rewrites
// only metadata segments of the JPEG byte buffer.
//
// Preservation contract:
//   * Every byte of the original TIFF payload after its 8-byte header is copied
//     verbatim, so each original value block (thumbnail JPEG, MakerNote,
//     strings) keeps its exact TIFF-relative offset and unknown maker-specific
//     internal offsets cannot be invalidated. Only the 8-byte header is
//     rewritten (byte order preserved, new IFD0 offset) and the IFD structures
//     are re-emitted in an appended region after the original payload.
//   * All other JPEG segments and the entropy-coded scan (SOS..EOI) are copied
//     byte for byte; only the first "Exif\0\0" APP1 segment is replaced.
//   * When neither a comment nor a description is requested the destination is
//     not written at all: destinationLength is set to sourceLength and the
//     caller emits the source buffer unchanged (byte-exact preservation).
//
// Bounds: at most MAX_IFD_ENTRIES entries per IFD, MAX_IFDS IFDs, MAX_SEGMENTS
// JPEG segments, 512 ASCII description bytes and a UserComment value of at most
// 256 bytes total (8-byte "ASCII\0\0\0" prefix + at most MAX_COMMENT_TEXT UTF-8
// bytes). Any violation fails closed with the destination untouched.
class QCamera3ExifExtras {
public:
    enum {
        MAX_COMMENT_TEXT = 248,   // UTF-8 text; value = prefix + text <= 256 bytes
        MAX_DESCRIPTION = 512,    // ASCII ImageDescription (the section 3.4 trace)
        MAX_JPEG_BYTES = 32 << 20,
        MAX_SEGMENTS = 256,
        MAX_IFDS = 6,
        MAX_IFD_ENTRIES = 64,
        MAX_APP1_PAYLOAD = 65533,
        MAX_GROWTH_BYTES = 8192,
        CANCEL_CHUNK = 65536,
        // ANDROID_CONTROL_AE_STATE_* values, repeated here so this module needs
        // no camera metadata header.
        AE_STATE_INACTIVE = 0,
        AE_STATE_SEARCHING = 1,
        AE_STATE_CONVERGED = 2,
        AE_STATE_LOCKED = 3,
        AE_STATE_FLASH_REQUIRED = 4,
        AE_STATE_PRECAPTURE = 5,
    };

    struct Options {
        // ASCII, printable, NUL-terminated, <= MAX_DESCRIPTION bytes. The
        // section 3.4 trace string is written here.
        const char *imageDescription = nullptr;
        // UTF-8, NUL-terminated, <= MAX_COMMENT_TEXT bytes. The caller must
        // terminate the org.lineageos.camera.*.exifComment byte[] payload.
        const char *userComment = nullptr;
        uint32_t frameNumber = 0;
        static bool valid(const Options &o);
    };

    struct Result {
        int32_t status = 0;   // 0 pass; negative errno otherwise
        int32_t applied = 0;  // 1 when metadata was written
        uint32_t frameNumber = 0;
        // detail[0] = preserved segment count, detail[1] = new APP1 payload
        // bytes, detail[2] = IFDs re-emitted, detail[3] = TIFF growth bytes.
        int32_t detail[4] = {};
        size_t sourceLength = 0;
        size_t outputLength = 0;
        uint32_t segments = 0;
        uint32_t ifds = 0;
        uint32_t app1Length = 0;
        uint8_t bypassed = 0;
        uint8_t createdApp1 = 0;      // no EXIF existed, one was inserted
        uint8_t structureChanged = 0; // an IFD entry was added or replaced
    };

    // Structural view for fixtures and tooling; no bytes are modified.
    struct JpegInfo {
        size_t exifMarkerOffset = 0;    // 0 when absent
        size_t exifSegmentLength = 0;   // marker + length field + payload
        size_t app1Count = 0;
        size_t segmentCount = 0;
        size_t scanOffset = 0;          // SOS marker offset
        uint32_t ifdCount = 0;
        uint32_t entryCount = 0;
        uint8_t hasExif = 0;
    };

    static const char *name();  // trace id "ExifExtras"
    static bool isAvailable();
    static bool validOptions(const Options &options);
    static bool validUtf8(const uint8_t *text, size_t length);
    // Upper bound of the output for a source of `sourceLength` bytes; the
    // caller sizes its destination buffer with this value.
    static size_t maxOutputLength(size_t sourceLength);
    static int32_t describe(const uint8_t *jpeg, size_t length, JpegInfo &info);

    // Rewrites the metadata of `source` into `destination`. On bypass and on
    // every error path the destination is untouched. destinationLength is only
    // meaningful on success. source and destination must not overlap.
    int32_t process(const uint8_t *source, size_t sourceLength, uint8_t *destination,
            size_t capacity, size_t &destinationLength, const Options &options,
            Result &result, const std::atomic<bool> *cancelled = nullptr);

    // No retained state exists; provided for the common stage shape.
    void reset() {}

    // --- professional / result info helpers (pure, bounded, no I/O) ---------
    // org.lineageos.camera.algorithms.professionalVer value.
    static int32_t professionalVer();
    // Real inputs only: the measured sensor exposure time of the last frame,
    // the snapshot frame duration and the advertised exposure range. AE must be
    // converged or locked; while AE searches the estimate is refused (-EAGAIN)
    // instead of being guessed.
    struct SnapshotExposureInput {
        int64_t sensorExposureTimeNs = 0;
        int64_t frameDurationNs = 0;
        int64_t maxExposureTimeNs = 0;
        int32_t aeState = AE_STATE_INACTIVE;
    };
    static int32_t estimateSnapshotExpTimeNs(const SnapshotExposureInput &input, int64_t &outNs);
    // LENS_FOCUS_DISTANCE guidance: pass through the real lens position in
    // dioptres (0 = infinity), clamp to the advertised range and report the
    // clamp with -ERANGE so the HAL can decide. Never synthesized from focal
    // length, face distance or depth. `out` is untouched on -EINVAL.
    static int32_t lensFocusDistance(float diopters, float minDiopters, float maxDiopters,
            float &out);
};

} // namespace qcamera
#endif
