/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3RawCapability.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace qcamera {
namespace {

// The stock calibration record is little-endian on both R11 and R11s.
inline uint32_t readU32LE(const uint8_t *p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
            (uint32_t(p[3]) << 24);
}

inline int32_t readI32LE(const uint8_t *p) {
    return int32_t(readU32LE(p));
}

inline bool finiteFloat(float value) {
    return value == value && value >= 0.0f && value <= 3.402823466e+38f;
}

inline bool powerOfTwo(uint32_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

bool validIlluminant(int32_t value) {
    // camera_metadata illuminant range (DAYLIGHT..ISO_STUDIO_TUNGSTEN).
    return value >= 1 && value <= 24;
}

bool matricesNonsingular(const QCamera3RawCapability::Rational *matrix) {
    double m[3][3];
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            const QCamera3RawCapability::Rational &entry = matrix[row * 3 + column];
            if (entry.denominator == 0) return false;
            m[row][column] = double(entry.numerator) / double(entry.denominator);
            if (!(m[row][column] == m[row][column])) return false;
        }
    }
    const double determinant =
            m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
            m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
            m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    return fabs(determinant) > 1e-9;
}

} // namespace

bool QCamera3RawCapability::FixedLevels::valid() const {
    if (known != 1 || whiteLevel == 0 || whiteLevel > 65535) return false;
    for (uint32_t i = 0; i < kBlackLevelChannels; ++i)
        if (blackLevelPattern[i] >= whiteLevel) return false;
    return true;
}

bool QCamera3RawCapability::supports(uint32_t width, uint32_t height) {
    return width > 0 && height > 0 && width <= kMaxDimension && height <= kMaxDimension;
}

size_t QCamera3RawCapability::dpcRecordSize(uint32_t modeIndex) {
    if (modeIndex >= kDpcModeCount) return 0;
    return size_t(modeIndex) * kDpcModeStride + kDpcCountOffset + 4;
}

bool QCamera3RawCapability::parseDpcRecord(const uint8_t *record, size_t size,
        uint32_t modeIndex, int32_t calibrationWidth, int32_t calibrationHeight,
        HotPixelMap &out) {
    if (!record || modeIndex >= kDpcModeCount) return false;
    if (calibrationWidth <= 0 || calibrationHeight <= 0) return false;
    if (calibrationWidth > int32_t(kMaxDimension) ||
            calibrationHeight > int32_t(kMaxDimension)) return false;
    const size_t required = dpcRecordSize(modeIndex);
    if (required == 0 || size < required) return false;
    const uint8_t *modeBase = record + size_t(modeIndex) * kDpcModeStride;
    const uint32_t count = readU32LE(modeBase + kDpcCountOffset);
    if (count > kDpcMaxCoordinates) return false;
    HotPixelMap parsed;
    parsed.count = count;
    parsed.calibrationWidth = calibrationWidth;
    parsed.calibrationHeight = calibrationHeight;
    parsed.modeIndex = int32_t(modeIndex);
    parsed.fromCalibration = 1;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *entry = modeBase + kDpcCoordinateOffset + size_t(i) * 8;
        const int32_t x = readI32LE(entry);
        const int32_t y = readI32LE(entry + 4);
        if (x < 0 || y < 0 || x >= calibrationWidth || y >= calibrationHeight) {
            return false;
        }
        parsed.coordinates[2 * i] = x;
        parsed.coordinates[2 * i + 1] = y;
    }
    out = parsed;
    return true;
}

bool QCamera3RawCapability::toActiveArray(const HotPixelMap &map, uint32_t activeWidth,
        uint32_t activeHeight, int32_t *coordinates, size_t capacity) {
    if (!coordinates || !map.onMode() || activeWidth == 0 || activeHeight == 0 ||
            capacity < size_t(map.count) * 2) return false;
    // Transactional: validate everything before writing.
    for (uint32_t i = 0; i < map.count; ++i) {
        const int64_t x = map.coordinates[2 * i];
        const int64_t y = map.coordinates[2 * i + 1];
        const int64_t scaledX = x * activeWidth / map.calibrationWidth;
        const int64_t scaledY = y * activeHeight / map.calibrationHeight;
        if (scaledX < 0 || scaledY < 0 || scaledX >= int64_t(activeWidth) ||
                scaledY >= int64_t(activeHeight)) return false;
    }
    for (uint32_t i = 0; i < map.count; ++i) {
        coordinates[2 * i] = int32_t(int64_t(map.coordinates[2 * i]) * activeWidth /
                map.calibrationWidth);
        coordinates[2 * i + 1] = int32_t(int64_t(map.coordinates[2 * i + 1]) * activeHeight /
                map.calibrationHeight);
    }
    return true;
}

bool QCamera3RawCapability::raw16Format(uint32_t width, uint32_t height,
        uint32_t strideAlignment, Raw16Format &out) {
    if (!supports(width, height) || !powerOfTwo(strideAlignment)) return false;
    const uint64_t rowBytes = uint64_t(width) * 2;
    const uint64_t mask = uint64_t(strideAlignment) - 1;
    if (rowBytes > UINT64_MAX - mask) return false;
    const uint64_t stride = (rowBytes + mask) & ~mask;
    if (stride > UINT32_MAX) return false;
    const uint64_t plane = stride * height;
    // The RAW16 plane is consumed as a 32-bit length; refuse rather than truncate.
    if (plane > UINT32_MAX) return false;
    Raw16Format format;
    format.width = width;
    format.height = height;
    format.strideBytes = uint32_t(stride);
    format.planeSizeBytes = plane;
    out = format;
    return true;
}

bool QCamera3RawCapability::raw16Offset(const Raw16Format &format, uint32_t x,
        uint32_t y, uint64_t *offset) {
    if (format.strideBytes == 0 || format.planeSizeBytes == 0 || !offset) return false;
    if (x >= format.width || y >= format.height) return false;
    const uint64_t end = uint64_t(y) * format.strideBytes + uint64_t(x) * 2 + 2;
    if (end < uint64_t(y) * format.strideBytes || end > format.planeSizeBytes) return false;
    *offset = end - 2;
    return true;
}

QCamera3RawCapability::Decision QCamera3RawCapability::decide(const Inputs &inputs) {
    Decision out;
    if (!supports(inputs.activeWidth, inputs.activeHeight) ||
            (inputs.activeWidth & 1u) || (inputs.activeHeight & 1u)) {
        out.status = -EINVAL; // malformed active array
        out.blockedBy |= kBlockedActiveArray;
        return out;
    }
    bool malformed = false;
    // Only real ON-mode coordinates count.
    if (inputs.hotPixel.fromCalibration == 1) {
        const HotPixelMap &map = inputs.hotPixel;
        if (map.count > kDpcMaxCoordinates || map.calibrationWidth <= 0 ||
                map.calibrationHeight <= 0) {
            malformed = true;
        } else {
            for (uint32_t i = 0; i < map.count && !malformed; ++i) {
                if (map.coordinates[2 * i] < 0 || map.coordinates[2 * i + 1] < 0 ||
                        map.coordinates[2 * i] >= map.calibrationWidth ||
                        map.coordinates[2 * i + 1] >= map.calibrationHeight)
                    malformed = true;
            }
        }
        out.detail[kDetailHotPixelMode] = map.modeIndex;
        if (map.calibrationWidth != int32_t(inputs.activeWidth) ||
                map.calibrationHeight != int32_t(inputs.activeHeight))
            out.detail[kDetailScaledCoordinates] = 1;
    } else {
        out.detail[kDetailHotPixelMode] = -1;
    }
    out.hotPixelMapAvailable = inputs.hotPixel.onMode() ? 1 : 0;
    if (!out.hotPixelMapAvailable) out.blockedBy |= kBlockedHotPixelMap;

    if (inputs.reflection.available == 1 && inputs.reflection.verifiedFormat != 0 &&
            inputs.reflection.count > 0) {
        out.reflectionMapAvailable = 1;
    } else {
        out.reflectionMapAvailable = 0;
        out.detail[kDetailReflectionUnverified] = 1;
    }

    // Dynamic black level from real metadata; fail closed.
    bool dynamicUsed = false;
    const float *values = nullptr;
    if (inputs.dynamicBlackLevel.appliedValid == 1)
        values = inputs.dynamicBlackLevel.applied;
    else if (inputs.dynamicBlackLevel.sourceValid == 1)
        values = inputs.dynamicBlackLevel.source;
    if (values) {
        bool usable = true;
        for (uint32_t i = 0; i < kBlackLevelChannels; ++i) {
            if (!finiteFloat(values[i]) || values[i] >= float(inputs.levels.whiteLevel))
                usable = false;
        }
        if (usable && inputs.levels.valid()) {
            dynamicUsed = true;
            for (uint32_t i = 0; i < kBlackLevelChannels; ++i)
                out.blackLevel[i] = uint32_t(values[i] + 0.5f);
        } else {
            out.detail[kDetailDynamicBlackLevelRejected] = 1;
        }
    }
    if (!dynamicUsed) {
        for (uint32_t i = 0; i < kBlackLevelChannels; ++i)
            out.blackLevel[i] = inputs.levels.blackLevelPattern[i];
    }
    out.dynamicBlackLevelUsed = dynamicUsed ? 1 : 0;

    // Two distinct illuminants with nonsingular rational matrices.
    bool colourOk = inputs.colour.matrixCount == kDynamicRangeCount &&
            validIlluminant(inputs.colour.illuminant[0]) &&
            validIlluminant(inputs.colour.illuminant[1]) &&
            inputs.colour.illuminant[0] != inputs.colour.illuminant[1] &&
            matricesNonsingular(inputs.colour.transform[0]) &&
            matricesNonsingular(inputs.colour.transform[1]);
    if (!colourOk) out.blockedBy |= kBlockedColour;
    if (inputs.sensitivityMax == 0 || inputs.noiseModelKnown != 1)
        out.blockedBy |= kBlockedNoiseModel;

    if (malformed) {
        out.status = -EINVAL; // fail closed
        out.raw16Available = 0;
        out.dngAvailable = 0;
        out.hotPixelMapAvailable = 0;
        out.completeRawCapability = 0;
        return out;
    }

    const bool levelsOk = inputs.levels.valid();
    if (!levelsOk) out.blockedBy |= kBlockedWhiteLevel | kBlockedBlackLevel;
    const bool raw16 = levelsOk && inputs.sensitivityMax > 0 &&
            inputs.noiseModelKnown == 1;
    out.raw16Available = raw16 ? 1 : 0;
    out.dngAvailable = raw16 && colourOk ? 1 : 0;
    out.completeRawCapability = (raw16 && colourOk && out.hotPixelMapAvailable == 1) ? 1 : 0;
    return out;
}

} // namespace qcamera
