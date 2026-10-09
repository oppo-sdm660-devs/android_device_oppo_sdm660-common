/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_EXIF_EXTRAS_H
#define QCAMERA3_EXIF_EXTRAS_H

#include <atomic>
#include <stddef.h>
#include <stdint.h>

namespace qcamera {

// EXIF UserComment / ImageDescription writer plus the professional-info helpers.
// Only metadata segments are rewritten; the rest of the TIFF payload is verbatim.
class QCamera3ExifExtras {
public:
    enum {
        MAX_COMMENT_TEXT = 248, // value = prefix + text <= 256 bytes
        MAX_DESCRIPTION = 512,  // ASCII ImageDescription
        MAX_JPEG_BYTES = 32 << 20,
        MAX_SEGMENTS = 256,
        MAX_IFDS = 6,
        MAX_IFD_ENTRIES = 64,
        MAX_APP1_PAYLOAD = 65533,
        MAX_GROWTH_BYTES = 8192,
        CANCEL_CHUNK = 65536,
        // ANDROID_CONTROL_AE_STATE_* values; no camera metadata header needed.
        AE_STATE_INACTIVE = 0,
        AE_STATE_SEARCHING = 1,
        AE_STATE_CONVERGED = 2,
        AE_STATE_LOCKED = 3,
        AE_STATE_FLASH_REQUIRED = 4,
        AE_STATE_PRECAPTURE = 5,
    };

    struct Options {
        // ASCII, NUL-terminated, <= MAX_DESCRIPTION bytes.
        const char *imageDescription = nullptr;
        // UTF-8, NUL-terminated, <= MAX_COMMENT_TEXT bytes.
        const char *userComment = nullptr;
        uint32_t frameNumber = 0;
        static bool valid(const Options &o);
    };

    struct Result {
        int32_t status = 0;   // 0 pass; negative errno otherwise
        int32_t applied = 0;
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
        uint8_t createdApp1 = 0;
        uint8_t structureChanged = 0;
    };

    struct JpegInfo {
        size_t exifMarkerOffset = 0;
        size_t exifSegmentLength = 0;   // marker + length field + payload
        size_t app1Count = 0;
        size_t segmentCount = 0;
        size_t scanOffset = 0;          // SOS marker offset
        uint32_t ifdCount = 0;
        uint32_t entryCount = 0;
        uint8_t hasExif = 0;
    };

    static const char *name();
    static bool isAvailable();
    static bool validOptions(const Options &options);
    static bool validUtf8(const uint8_t *text, size_t length);
    // Upper bound; the caller sizes its destination buffer with this value.
    static size_t maxOutputLength(size_t sourceLength);
    static int32_t describe(const uint8_t *jpeg, size_t length, JpegInfo &info);

    // Destination is untouched on bypass and on every error path; destinationLength
    // is only meaningful on success. source and destination must not overlap.
    int32_t process(const uint8_t *source, size_t sourceLength, uint8_t *destination,
            size_t capacity, size_t &destinationLength, const Options &options,
            Result &result, const std::atomic<bool> *cancelled = nullptr);

    void reset() {}

    // org.lineageos.camera.algorithms.professionalVer.
    static int32_t professionalVer();
    // Sensor exposure time, frame duration and advertised range; AE must be
    // converged or locked, otherwise the estimate is refused with -EAGAIN.
    struct SnapshotExposureInput {
        int64_t sensorExposureTimeNs = 0;
        int64_t frameDurationNs = 0;
        int64_t maxExposureTimeNs = 0;
        int32_t aeState = AE_STATE_INACTIVE;
    };
    static int32_t estimateSnapshotExpTimeNs(const SnapshotExposureInput &input, int64_t &outNs);
    // Dioptres (0 = infinity); clamps to the advertised range and reports -ERANGE,
    // `out` is untouched on -EINVAL.
    static int32_t lensFocusDistance(float diopters, float minDiopters, float maxDiopters,
            float &out);
};

} // namespace qcamera
#endif
