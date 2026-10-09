/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_HDR_PLAN_H
#define QCAMERA3_HDR_PLAN_H

#include <stdint.h>

namespace qcamera {

// Rational sensor step; ArcSoft receives base/low/high images.
struct QCamera3HdrPlan {
    enum { SETTLE_FRAMES = 2, FRAME_COUNT = 3 * (SETTLE_FRAMES + 1) };
    int32_t exposure[3];

    static bool manualExposures(int64_t minimum, int64_t maximum,
            int64_t baseline, int64_t (&times)[3]) {
        if (minimum <= 0 || maximum < minimum || baseline < minimum || baseline > maximum)
            return false;
        times[0] = baseline;
        times[1] = baseline / 4 < minimum ? minimum : baseline / 4;
        times[2] = baseline > maximum / 4 ? maximum : baseline * 4;
        return double(times[1]) < double(baseline) * 0.95 &&
                double(times[2]) > double(baseline) * 1.05;
    }

    static bool make(int32_t minimum, int32_t maximum, int32_t numerator,
            int32_t denominator, int32_t base, QCamera3HdrPlan &plan) {
        if (numerator <= 0 || denominator <= 0 || minimum >= maximum ||
                base < minimum || base > maximum) return false;
        // Nearest representable two-stop bracket; all arithmetic is widened.
        int64_t ticks = (int64_t(2) * denominator + numerator / 2) / numerator;
        if (ticks < 1) ticks = 1;
        const int64_t low = int64_t(base) - ticks;
        const int64_t high = int64_t(base) + ticks;
        plan.exposure[0] = base;
        plan.exposure[1] = low < minimum ? minimum : int32_t(low);
        plan.exposure[2] = high > maximum ? maximum : int32_t(high);
        return plan.exposure[1] < base && plan.exposure[2] > base;
    }
};

// LLHDR: four normal exposures and one short one, using the stock -2 EV
// variant; the -3 EV private bracket exceeds the public AE range.
struct QCamera3LowLightPlan {
    enum { SETTLE_FRAMES = 2, IMAGE_COUNT = 5,
        FRAME_COUNT = IMAGE_COUNT * (SETTLE_FRAMES + 1) };
    int32_t exposure[IMAGE_COUNT];
    static bool make(int32_t minimum, int32_t maximum, int32_t numerator,
            int32_t denominator, int32_t base, QCamera3LowLightPlan &plan) {
        if (numerator <= 0 || denominator <= 0 || minimum >= maximum ||
                base < minimum || base > maximum) return false;
        const int64_t ticks = (int64_t(2) * denominator + numerator / 2) / numerator;
        const int64_t shortExposure = int64_t(base) - ticks;
        if (ticks < 1 || shortExposure < minimum || shortExposure >= base) return false;
        for (unsigned i = 0; i < IMAGE_COUNT - 1; ++i) plan.exposure[i] = base;
        plan.exposure[IMAGE_COUNT - 1] = int32_t(shortExposure);
        return true;
    }
};

} // namespace qcamera
#endif
