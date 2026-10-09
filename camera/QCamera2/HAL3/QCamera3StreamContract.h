/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_STREAM_CONTRACT_H
#define QCAMERA3_STREAM_CONTRACT_H

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace qcamera {

// Pixel/metadata contracts shared by publication and admission.
class QCamera3StreamContract {
public:
    struct Rational { int32_t numerator, denominator; };
    struct RawCalibration {
        bool singleCamera, raw10Packing, matchingCfa, knownIlluminants;
        uint32_t channels, pixelWidth, pixelHeight;
        int32_t activeLeft, activeTop, activeWidth, activeHeight;
        int32_t whiteLevel, blackLevel[4];
        int64_t maxFrameDuration;
        Rational matrices[6][9];
    };
    static bool nonsingular(const Rational matrix[9]) {
        double values[9];
        for (unsigned i = 0; i < 9; ++i) {
            if (!matrix[i].denominator) return false;
            values[i] = double(matrix[i].numerator) / matrix[i].denominator;
        }
        const double determinant = values[0] * (values[4] * values[8] - values[5] * values[7]) -
                values[1] * (values[3] * values[8] - values[5] * values[6]) +
                values[2] * (values[3] * values[7] - values[4] * values[6]);
        return std::isfinite(determinant) && std::fabs(determinant) > 1e-12;
    }
    static bool rawCalibration(const RawCalibration &raw) {
        if (!raw.singleCamera || !raw.raw10Packing || !raw.matchingCfa || !raw.knownIlluminants ||
                raw.channels != 4 || !raw.pixelWidth || !raw.pixelHeight ||
                raw.pixelWidth > 16384 || raw.pixelHeight > 16384 ||
                raw.activeLeft < 0 || raw.activeTop < 0 || raw.activeWidth <= 0 || raw.activeHeight <= 0 ||
                int64_t(raw.activeLeft) + raw.activeWidth > raw.pixelWidth ||
                int64_t(raw.activeTop) + raw.activeHeight > raw.pixelHeight ||
                raw.whiteLevel != 1023 || raw.maxFrameDuration <= 0) return false;
        for (int32_t black : raw.blackLevel)
            if (black < 0 || black >= raw.whiteLevel) return false;
        for (const auto &matrix : raw.matrices)
            if (!nonsingular(matrix)) return false;
        return true;
    }
    static bool rawSize(const RawCalibration &raw, uint32_t width, uint32_t height,
            int64_t minimumDuration, int64_t stall) {
        return width && height && !((width | height) & 1) &&
                width <= raw.pixelWidth && height <= raw.pixelHeight &&
                minimumDuration > 0 && minimumDuration <= raw.maxFrameDuration && stall >= 0;
    }
    static bool reprocessCrop(uint32_t width, uint32_t height, const int32_t *crop,
            size_t count, const int32_t *roi, size_t roiCount) {
        if (!width || !height || !crop || count != 4 || !roi || roiCount != 4 ||
                crop[0] < 0 || crop[1] < 0 || crop[2] <= 0 || crop[3] <= 0 ||
                int64_t(crop[0]) + crop[2] > width ||
                int64_t(crop[1]) + crop[3] > height ||
                roi[0] < 0 || roi[1] < 0 || roi[2] <= 0 || roi[3] <= 0 ||
                int64_t(roi[0]) + roi[2] > INT32_MAX ||
                int64_t(roi[1]) + roi[3] > INT32_MAX) return false;
        return true;
    }
    static bool reprocessMetadata(size_t privateWords, size_t expectedPrivateWords,
            size_t cropCountEntries, int32_t cropCount, size_t cropValues, size_t roiValues,
            size_t timestampEntries, int64_t timestamp, float exposureFactor) {
        return privateWords == expectedPrivateWords && expectedPrivateWords &&
                cropCountEntries == 1 && cropCount == 1 && cropValues == 4 && roiValues == 4 &&
                timestampEntries == 1 && timestamp > 0 &&
                std::isfinite(exposureFactor) && exposureFactor > 0;
    }
    // The input belongs to every output consumer; return it once the final accepted
    // output completes. Duplicate/foreign completions cannot count.
    static bool completeReprocessOutput(uint32_t &remaining, bool &metadataSent,
            bool &sendMetadata, bool &returnInput) {
        sendMetadata = returnInput = false;
        if (!remaining) return false;
        sendMetadata = !metadataSent;
        metadataSent = true;
        returnInput = --remaining == 0;
        return true;
    }
};
} // namespace qcamera
#endif
