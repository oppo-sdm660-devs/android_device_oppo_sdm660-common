/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_RAW10_UNPACK_H
#define QCAMERA3_RAW10_UNPACK_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace qcamera {
struct QCamera3Raw10Unpack {
    enum Packing { MIPI10, LEGACY10 };
    // RAW16 output is little-endian, meaningful samples in bits 9..0.
    // Each packed group is copied before expansion.
    static bool inPlace(void *buffer, size_t allocationSize, size_t inputSize,
            uint32_t width, uint32_t height, size_t inputStride,
            size_t outputStride, Packing packing) {
        if (!buffer || !width || !height || inputSize > allocationSize ||
                (packing != MIPI10 && packing != LEGACY10)) return false;
        const size_t pixelsPerGroup = packing == MIPI10 ? 4 : 6;
        const size_t bytesPerGroup = packing == MIPI10 ? 5 : 8;
        const uint64_t groups = (uint64_t(width) + pixelsPerGroup - 1) / pixelsPerGroup;
        const uint64_t rowBytes = groups * bytesPerGroup;
        if (inputStride < rowBytes || outputStride < uint64_t(width) * 2 ||
                outputStride < inputStride || outputStride > SIZE_MAX / height ||
                inputStride > SIZE_MAX / height) return false;
        const size_t requiredInput = (height - 1) * inputStride + size_t(rowBytes);
        const size_t requiredOutput = height * outputStride;
        if (requiredInput > inputSize || requiredOutput > allocationSize) return false;
        uint8_t *bytes = static_cast<uint8_t *>(buffer);
        for (uint32_t y = height; y-- != 0;) {
            const uint8_t *source = bytes + y * inputStride;
            uint8_t *destination = bytes + y * outputStride;
            for (size_t group = size_t(groups); group-- != 0;) {
                uint8_t packed[8] = {};
                memcpy(packed, source + group * bytesPerGroup, bytesPerGroup);
                uint64_t legacy = 0;
                if (packing == LEGACY10)
                    for (size_t byte = 0; byte < 8; ++byte)
                        legacy |= uint64_t(packed[byte]) << (8 * byte);
                for (size_t pixel = pixelsPerGroup; pixel-- != 0;) {
                    const size_t x = group * pixelsPerGroup + pixel;
                    if (x >= width) continue;
                    const uint16_t value = packing == MIPI10 ?
                            (uint16_t(packed[pixel]) << 2) |
                            ((packed[4] >> (2 * pixel)) & 3) :
                            (legacy >> (10 * pixel)) & 0x3ff;
                    destination[2 * x] = value;
                    destination[2 * x + 1] = value >> 8;
                }
            }
            memset(destination + size_t(width) * 2, 0, outputStride - size_t(width) * 2);
        }
        return true;
    }
};
}
#endif
