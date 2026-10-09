/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_RAW_CAPABILITY_H
#define QCAMERA3_RAW_CAPABILITY_H

#include <stddef.h>
#include <stdint.h>

namespace qcamera {

// Complete RAW/DNG capability admission. Everything here is decided from real
// decoded calibration and metadata: a real hot-pixel (defect-pixel) record read
// with the stock calibration layout, the real fixed black/white levels, the real
// per-frame dynamic black level and the real per-illuminant colour matrices.
//
// The rule is fail-closed: a capability that is not backed by real data stays
// unavailable. In particular the framework's complete
// REQUEST_AVAILABLE_CAPABILITIES_RAW contract needs hot-pixel map ON mode, and
// when the real ON-mode coordinates are missing the decision reports
// hotPixelMapAvailable=0 and the caller must keep RAW capability limited.
class QCamera3RawCapability {
public:
    // Stock calibration layout, verified from libmmcamera2_sensor_modules.so
    // function dump_dpc_data (see docs/RAW_CAPABILITY.md for the exact
    // instructions): per-mode records of 512 int32 (x,y) pairs followed by an
    // int32 count, 14 resolution modes, mode stride 0x1004.
    static constexpr uint32_t kDpcModeCount = 14;
    static constexpr uint32_t kDpcMaxCoordinates = 512;
    static constexpr size_t kDpcModeStride = 0x1004;
    static constexpr size_t kDpcCoordinateOffset = 0x3974;
    static constexpr size_t kDpcCountOffset = 0x4974;

    enum {
        kMaxDimension = 16384,
        kBlackLevelChannels = 4,
        kMatrixEntries = 9,
        kDynamicRangeCount = 2,
    };

    // Decision blockers, reported so the caller knows what to keep limited.
    enum Blocker {
        kBlockedActiveArray = 1 << 0,
        kBlockedWhiteLevel = 1 << 1,
        kBlockedBlackLevel = 1 << 2,
        kBlockedNoiseModel = 1 << 3,
        kBlockedColour = 1 << 4,
        kBlockedHotPixelMap = 1 << 5,
    };
    // Decision::detail slots (module-specific, stable).
    enum Detail {
        kDetailDynamicBlackLevelRejected = 0, // 1 => metadata ignored, fail closed
        kDetailReflectionUnverified = 1,      // 1 => no verified reflection source
        kDetailHotPixelMode = 2,              // decoded calibration mode, -1 none
        kDetailScaledCoordinates = 3,         // coordinates scaled to active array
    };

    struct Rational {
        int32_t numerator = 0;
        int32_t denominator = 0;
    };

    // Hot-pixel coordinates in the real calibration resolution. `onMode()` is
    // the framework's ON-mode admission.
    struct HotPixelMap {
        uint32_t count = 0;
        int32_t coordinates[2 * kDpcMaxCoordinates] = {};
        int32_t calibrationWidth = 0;
        int32_t calibrationHeight = 0;
        int32_t modeIndex = -1;
        int32_t fromCalibration = 0;

        bool onMode() const {
            return fromCalibration == 1 && count > 0 && calibrationWidth > 0 &&
                    calibrationHeight > 0;
        }
    };

    // Reflection-map coordinates are accepted only together with a verified
    // source format id; no stock R11/R11s reflection record exists, so a caller
    // that has no verified source must leave this unavailable.
    struct ReflectionMap {
        int32_t available = 0;
        int32_t verifiedFormat = 0;   // 0 = unknown source, refused
        uint32_t count = 0;
    };

    struct FixedLevels {
        uint32_t whiteLevel = 0;                          // real, e.g. 1023
        uint32_t blackLevelPattern[kBlackLevelChannels] = {}; // real per-CFA-channel
        int32_t known = 0;

        bool valid() const;
    };

    struct DynamicBlackLevel {
        float source[kBlackLevelChannels] = {};
        float applied[kBlackLevelChannels] = {};
        int32_t sourceValid = 0;   // ANDROID_SENSOR_DYNAMIC_BLACK_LEVEL (request)
        int32_t appliedValid = 0;  // applied dynamic black level (result)
    };

    struct ColourCalibration {
        int32_t illuminant[kDynamicRangeCount] = {};                 // real enum ids
        Rational transform[kDynamicRangeCount][kMatrixEntries] = {}; // COLOR_TRANSFORM1/2
        uint32_t matrixCount = 0;                                    // 2 = complete
    };

    struct Inputs {
        uint32_t activeWidth = 0;
        uint32_t activeHeight = 0;
        FixedLevels levels;
        HotPixelMap hotPixel;
        ReflectionMap reflection;
        DynamicBlackLevel dynamicBlackLevel;
        ColourCalibration colour;
        uint32_t sensitivityMax = 0;   // real maximum analog sensitivity
        int32_t noiseModelKnown = 0;   // real noise model coefficients decoded
    };

    struct Decision {
        int32_t status = 0;            // 0 = well formed; -EINVAL otherwise
        int32_t raw16Available = 0;
        int32_t dngAvailable = 0;
        int32_t hotPixelMapAvailable = 0;
        int32_t reflectionMapAvailable = 0;
        int32_t dynamicBlackLevelUsed = 0;
        int32_t completeRawCapability = 0;
        uint32_t blackLevel[kBlackLevelChannels] = {};
        int32_t blockedBy = 0;
        int32_t detail[4] = {};
    };

    struct Raw16Format {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t strideBytes = 0;
        uint64_t planeSizeBytes = 0;
    };

    static const char *name() { return "RawCapability"; }
    static bool isAvailable() { return true; }
    static bool supports(uint32_t width, uint32_t height);

    // Real calibration parser. `record` is the stock calibration image; the
    // per-mode defect-pixel record is selected by modeIndex. Returns false and
    // leaves `out` untouched on malformed data.
    static size_t dpcRecordSize(uint32_t modeIndex);
    static bool parseDpcRecord(const uint8_t *record, size_t size, uint32_t modeIndex,
            int32_t calibrationWidth, int32_t calibrationHeight, HotPixelMap &out);

    // Scales calibration coordinates onto the real active array (ON-mode
    // coordinates are active-array coordinates). Capacity is in int32 entries
    // (2 * count needed). Overflow-safe, truncating and documented.
    static bool toActiveArray(const HotPixelMap &map, uint32_t activeWidth,
            uint32_t activeHeight, int32_t *coordinates, size_t capacity);

    // Exact, overflow-safe RAW16 geometry helpers.
    static bool raw16Format(uint32_t width, uint32_t height, uint32_t strideAlignment,
            Raw16Format &out);
    static bool raw16Offset(const Raw16Format &format, uint32_t x, uint32_t y,
            uint64_t *offset);

    static Decision decide(const Inputs &inputs);
};

} // namespace qcamera
#endif
