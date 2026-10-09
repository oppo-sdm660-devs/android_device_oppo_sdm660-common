/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_VIDEO_GRADES_H
#define QCAMERA3_VIDEO_GRADES_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>

namespace qcamera {

/*
 * Video grades Aperture can actually select.
 *
 * android.control.aeAvailableTargetFpsRanges is what CameraX reports to
 * Aperture as CameraInfo.supportedFrameRateRanges, and Aperture turns one entry
 * into a selectable frame-rate chip only when that entry is *fixed*:
 * FrameRate.fromRange() maps [n,n] to n and drops every [a,b] with a < b. The
 * decoded vendor table (cam_capability_t::fps_ranges_tbl) is stored as (min,max)
 * pairs and routinely carries the usable rates only inside variable ranges, so
 * publishing it verbatim hides them: a camera whose table is {[15,30]} offers no
 * frame-rate chip at all, even though 30 fps is a rate the sensor itself
 * declares.
 *
 * This policy derives the published list from the decoded capability only; it
 * never invents a grade. Three groups, in order:
 *   1. the decoded (min,max) pairs, verbatim and in vendor order (the HAL's own
 *      default request templates scan that same decoded table, so the leading
 *      entries must keep their meaning);
 *   2. the fixed endpoints (min,min) and (max,max) of every decoded pair. A
 *      fixed rate inside a declared range is a sub-range of a mode the sensor
 *      already supports, and accepts() admits exactly those sub-ranges;
 *   3. [60,60] when a decoded 60 fps HFR sensor tier (CAM_HFR_MODE_60FPS)
 *      exists at 1920x1080 or larger. That tier is the normal, non-batch 60 fps
 *      recording route the vendor stack declares; >=120 fps tiers stay
 *      high-speed-only and keep their own advertisement.
 * The result is byte-for-byte the vendor list when the capability has neither
 * whole-number endpoints nor a 60 fps tier at 1080p or above, and it is capped
 * at kMaxRanges pairs like the table it replaces.
 */
struct QCamera3VideoGrades {
    /* A published target-FPS range. */
    struct Range {
        int32_t minFps;
        int32_t maxFps;
    };
    /* One decoded cam_capability_t::fps_ranges_tbl entry. */
    struct VendorRange {
        float minFps;
        float maxFps;
    };
    /* One decoded dimension of a CAM_HFR_MODE_60FPS tier. */
    struct SixtyTierSize {
        int32_t width;
        int32_t height;
    };

    /* Mirror of MAX_SIZES_CNT: the number of (min,max) pairs the framework list
     * was bounded by before this policy existed. */
    static const size_t kMaxRanges = 40;
    /* A 60 fps tier only counts as a video grade from 1080p up: the published
     * list is camera-wide, while a 720p-only tier would still put a 60 chip on
     * the FHD quality, which is not what the sensor declared. */
    static const int32_t kSixtyTierMinWidth = 1920;
    static const int32_t kSixtyTierMinHeight = 1080;

    static bool publish(const Range *ranges, size_t count, int32_t minFps,
            int32_t maxFps, Range *out, size_t capacity, size_t *outCount) {
        if (out == NULL || outCount == NULL || minFps <= 0 || maxFps < minFps)
            return false;
        for (size_t i = 0; i < count; ++i)
            if (ranges[i].minFps == minFps && ranges[i].maxFps == maxFps)
                return false; // duplicate, nothing added
        if (count >= capacity) return false;
        out[count].minFps = minFps;
        out[count].maxFps = maxFps;
        *outCount = count + 1;
        return true;
    }

    /* Derive the published list. Returns the number of pairs written to out. */
    static size_t build(const VendorRange *vendor, size_t vendorCount,
            const SixtyTierSize *sixtyTier, size_t sixtyTierCount, Range *out,
            size_t capacity) {
        if (out == NULL || capacity == 0) return 0;
        size_t count = 0;
        if (vendor == NULL) vendorCount = 0;
        if (vendorCount > kMaxRanges) vendorCount = kMaxRanges;
        if (sixtyTier == NULL) sixtyTierCount = 0;
        for (size_t i = 0; i < vendorCount; ++i) {
            /* Truncation keeps the vendor's own rounding for fractional rates;
             * the result is only published when it survives the ordering test. */
            size_t next = count;
            if (publish(out, count, int32_t(vendor[i].minFps),
                    int32_t(vendor[i].maxFps), out, capacity, &next))
                count = next;
        }
        for (size_t i = 0; i < vendorCount; ++i) {
            const float endpoints[] = {vendor[i].minFps, vendor[i].maxFps};
            for (size_t j = 0; j < sizeof(endpoints) / sizeof(endpoints[0]); ++j) {
                /* Whole-number rates only: the framework range is integral and a
                 * truncated fraction (7.5 fps) would name a rate the vendor
                 * never declared. */
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

    /* True when minFps..maxFps is advertised exactly, or is a sub-range of an
     * advertised range: a fixed rate held inside a variable range is the same
     * sensor mode at a constant rate, which is what makes the derived fixed
     * entries in build() requestable. Anything outside every advertised range is
     * refused, so a chip the HAL never published can never reach the sensor. */
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
