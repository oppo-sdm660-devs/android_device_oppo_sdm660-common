/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_MMS_GL_MESH_H
#define QCAMERA3_MMS_GL_MESH_H

/* Topology of the stock MMS warper mesh: one glDrawElements(GL_TRIANGLE_STRIP,
 * GL_UNSIGNED_SHORT) over a regular H x V grid; dependency free. */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace qcamera {
namespace mmsgl {

/* Stock index count for an H x V grid; 0 for a degenerate grid. */
inline size_t stripIndexCount(uint32_t columns, uint32_t rows) {
    if (columns < 2 || rows < 2) return 0;
    return size_t(columns) * rows + size_t(rows - 2) * (columns - 1);
}

/* Regular row-major UV grid; row zero is the first buffer row (no vertical flip). */
inline void buildUvGrid(float *uv, uint32_t columns, uint32_t rows) {
    if (!uv || columns < 2 || rows < 2) return;
    for (uint32_t j = 0; j < rows; ++j) {
        const float v = static_cast<float>(j) / static_cast<float>(rows - 1);
        for (uint32_t i = 0; i < columns; ++i) {
            uv[(size_t(j) * columns + i) * 2 + 0] =
                    static_cast<float>(i) / static_cast<float>(columns - 1);
            uv[(size_t(j) * columns + i) * 2 + 1] = v;
        }
    }
}

/* Fills indices; returns entries written, 0 when no valid strip can be built. */
inline size_t buildStripIndices(uint16_t *indices, size_t capacity,
        uint32_t columns, uint32_t rows) {
    const size_t count = stripIndexCount(columns, rows);
    if (!indices || capacity < count || count == 0 ||
            size_t(columns) * rows > 0x10000u) return 0;
    size_t k = 0;
    /* Row pair (0, 1), left to right. */
    for (uint32_t i = 0; i < columns; ++i) {
        indices[k++] = uint16_t(0 * columns + i);
        indices[k++] = uint16_t(1 * columns + i);
    }
    /* Row pair (j, j+1): one collinear seam element plus the 2(H-1) alternated
     * ones; the seam vertex is an end vertex of the new row j+1. */
    for (uint32_t j = 1; j + 1 < rows; ++j) {
        if (j & 1u) {
            indices[k++] = uint16_t((j + 1) * columns + (columns - 1));
            for (uint32_t i = columns - 1; i-- > 0;) {
                indices[k++] = uint16_t(j * columns + i);
                indices[k++] = uint16_t((j + 1) * columns + i);
            }
        } else {
            indices[k++] = uint16_t((j + 1) * columns + 0);
            for (uint32_t i = 1; i < columns; ++i) {
                indices[k++] = uint16_t(j * columns + i);
                indices[k++] = uint16_t((j + 1) * columns + i);
            }
        }
    }
    return k == count ? k : 0;
}

/* Checks element count, triangle/quad coverage. Constructor-time only;
 * returns 0, -EINVAL or -EPROTO. */
inline int32_t selfCheck(uint32_t columns, uint32_t rows, uint16_t *indices,
        uint8_t *scratch) {
    const size_t count = stripIndexCount(columns, rows);
    if (!indices || !scratch || count == 0) return -EINVAL;
    if (buildStripIndices(indices, count, columns, rows) != count) return -EPROTO;
    memset(scratch, 0, size_t(columns) * rows);
    const size_t vertices = size_t(columns) * rows;
    size_t real = 0, degenerate = 0;
    for (size_t k = 0; k + 2 < count; ++k) {
        const uint16_t a = indices[k], b = indices[k + 1], c = indices[k + 2];
        if (a >= vertices || b >= vertices || c >= vertices) return -EPROTO;
        const int64_t x1 = a % columns, y1 = a / columns;
        const int64_t x2 = b % columns, y2 = b / columns;
        const int64_t x3 = c % columns, y3 = c / columns;
        const int64_t area = (x2 - x1) * (y3 - y1) - (x3 - x1) * (y2 - y1);
        if (area == 0) { ++degenerate; continue; }
        const int64_t minX = x1 < x2 ? (x1 < x3 ? x1 : x3) : (x2 < x3 ? x2 : x3);
        const int64_t minY = y1 < y2 ? (y1 < y3 ? y1 : y3) : (y2 < y3 ? y2 : y3);
        const int64_t maxX = x1 > x2 ? (x1 > x3 ? x1 : x3) : (x2 > x3 ? x2 : x3);
        const int64_t maxY = y1 > y2 ? (y1 > y3 ? y1 : y3) : (y2 > y3 ? y2 : y3);
        if (maxX - minX > 1 || maxY - minY > 1) return -EPROTO;
        ++real;
        ++scratch[size_t(minY) * columns + size_t(minX)];
    }
    if (real != size_t(2) * (columns - 1) * (rows - 1)) return -EPROTO;
    if (degenerate != size_t(rows - 2)) return -EPROTO;
    for (uint32_t qy = 0; qy + 1 < rows; ++qy)
        for (uint32_t qx = 0; qx + 1 < columns; ++qx)
            if (scratch[size_t(qy) * columns + qx] != 2) return -EPROTO;
    return 0;
}

} // namespace mmsgl
} // namespace qcamera
#endif
