/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_PAIRED_SESSION_POLICY_H
#define QCAMERA3_PAIRED_SESSION_POLICY_H

#include <stddef.h>
#include <stdint.h>

namespace qcamera {
struct QCamera3PairedSessionPolicy {
    struct Rect { int32_t left, top, width, height; };
    // Public dimensions are checked against the main capability table by HWI.
    // JPEG uses independent internal full-FOV NV21 sources; preview owns only
    // the main stream and need not have the same dimensions as the JPEG.
    static bool acceptsOutput(uint32_t profileWidth, uint32_t profileHeight,
            uint32_t width, uint32_t height, bool /*video*/, bool rotated) {
        return profileWidth != 0 && profileHeight != 0 && width != 0 && height != 0 &&
                width <= profileWidth && height <= profileHeight && !((width | height) & 1) &&
                !rotated;
    }
    static bool acceptsCombination(size_t jpegCount, size_t processedCount) {
        return jpegCount <= 1 && processedCount <= 3 && jpegCount + processedCount != 0;
    }
    // The measured active-array crop and sensor-output crop describe the same
    // field of view. Clip metering rectangles before mapping their edges.
    static bool mapRect(const Rect &from, const Rect &to, Rect &value) {
        if (from.width <= 0 || from.height <= 0 || to.width <= 0 || to.height <= 0 ||
                value.width <= 0 || value.height <= 0) return false;
        const int64_t left = value.left > from.left ? value.left : from.left;
        const int64_t top = value.top > from.top ? value.top : from.top;
        const int64_t r = int64_t(value.left) + value.width;
        const int64_t b = int64_t(value.top) + value.height;
        const int64_t fromRight = int64_t(from.left) + from.width;
        const int64_t fromBottom = int64_t(from.top) + from.height;
        const int64_t right = r < fromRight ? r : fromRight;
        const int64_t bottom = b < fromBottom ? b : fromBottom;
        if (right <= left || bottom <= top) return false;
        const int64_t x = to.left + (left - from.left) * to.width / from.width;
        const int64_t y = to.top + (top - from.top) * to.height / from.height;
        const int64_t endX = to.left + ((right - from.left) * to.width + from.width - 1) / from.width;
        const int64_t endY = to.top + ((bottom - from.top) * to.height + from.height - 1) / from.height;
        if (x < INT32_MIN || y < INT32_MIN || endX > INT32_MAX || endY > INT32_MAX ||
                endX <= x || endY <= y) return false;
        value = {int32_t(x), int32_t(y), int32_t(endX - x), int32_t(endY - y)};
        return true;
    }
};
}
#endif
