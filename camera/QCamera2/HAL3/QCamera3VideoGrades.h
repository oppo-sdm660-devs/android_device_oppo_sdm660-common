/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_VIDEO_GRADES_H
#define QCAMERA3_VIDEO_GRADES_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>

namespace qcamera {

// Video grades Aperture can select: only fixed [n,n] ranges become chips, so the
// endpoints of the decoded variable ranges are derived here. Order: decoded pairs,
// then endpoints, then 60 fps when a decoded HFR tier exists at 1080p or larger.
struct QCamera3VideoGrades {
    /* Published target-FPS range. */
    struct Range {
        int32_t minFps;
        int32_t maxFps;
    };
    /* One decoded cam_capability_t::fps_ranges_tbl entry. */
    struct VendorRange {
        float minFps;
        float maxFps;
    };
    /* Decoded dimension of a CAM_HFR_MODE_60FPS tier. */
    struct SixtyTierSize {
        int32_t width;
        int32_t height;
    };

    /* Mirrors MAX_SIZES_CNT, the bound of the list this policy replaces. */
    static const size_t kMaxRanges = 40;
    /* A 60 fps tier counts from 1080p up: the published list is camera-wide. */
    static const int32_t kSixtyTierMinWidth = 1920;
    static const int32_t kSixtyTierMinHeight = 1080;

    static bool publish(const Range *ranges, size_t count, int32_t minFps,
            int32_t maxFps, Range *out, size_t capacity, size_t *outCount) {
        if (out == NULL || outCount == NULL || minFps <= 0 || maxFps < minFps)
            return false;
        for (size_t i = 0; i < count; ++i)
            if (ranges[i].minFps == minFps && ranges[i].maxFps == maxFps)
                return false;
        if (count >= capacity) return false;
        out[count].minFps = minFps;
        out[count].maxFps = maxFps;
        *outCount = count + 1;
        return true;
    }

    /* Derives the published list; returns the number of pairs written. */
    static size_t build(const VendorRange *vendor, size_t vendorCount,
            const SixtyTierSize *sixtyTier, size_t sixtyTierCount, Range *out,
            size_t capacity) {
        if (out == NULL || capacity == 0) return 0;
        size_t count = 0;
        if (vendor == NULL) vendorCount = 0;
        if (vendorCount > kMaxRanges) vendorCount = kMaxRanges;
        if (sixtyTier == NULL) sixtyTierCount = 0;
        for (size_t i = 0; i < vendorCount; ++i) {
            /* Truncation keeps the vendor's rounding; only published if the order holds. */
            size_t next = count;
            if (publish(out, count, int32_t(vendor[i].minFps),
                    int32_t(vendor[i].maxFps), out, capacity, &next))
                count = next;
        }
        for (size_t i = 0; i < vendorCount; ++i) {
            const float endpoints[] = {vendor[i].minFps, vendor[i].maxFps};
            for (size_t j = 0; j < sizeof(endpoints) / sizeof(endpoints[0]); ++j) {
                /* Whole-number rates only: the framework range is integral. */
                if (endpoints[j] < 1.0f || endpoints[j] != floorf(endpoints[j]))
                    continue;
                size_t next = count;
                if (publish(out, count, int32_t(endpoints[j]),
                        int32_t(endpoints[j]), out, capacity, &next))
                    count = next;
            }
        }
        bool sixtyFpsTier = false;
        for (size_t i = 0; i < sixtyTierCount && !sixtyFpsTier; ++i)
            sixtyFpsTier = sixtyTier[i].width >= kSixtyTierMinWidth &&
                    sixtyTier[i].height >= kSixtyTierMinHeight;
        if (sixtyFpsTier) {
            size_t next = count;
            if (publish(out, count, 60, 60, out, capacity, &next)) count = next;
        }
        return count;
    }

    /* True when the range is advertised exactly or is a sub-range of an advertised range. */
    static bool accepts(const Range *advertised, size_t count, int32_t minFps,
            int32_t maxFps) {
        if (advertised == NULL || minFps <= 0 || maxFps < minFps) return false;
        for (size_t i = 0; i < count; ++i) {
            if (advertised[i].minFps == minFps && advertised[i].maxFps == maxFps)
                return true;
            if (advertised[i].minFps <= minFps && maxFps <= advertised[i].maxFps)
                return true;
        }
        return false;
    }
};

} // namespace qcamera

#endif // QCAMERA3_VIDEO_GRADES_H
