/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_HDR_PLAN_H
#define QCAMERA3_HDR_PLAN_H

#include <stdint.h>

namespace qcamera {

// Exposure compensation uses the sensor's rational step, not the private
// stock wrapper's six-frame table. ArcSoft receives base/low/high images.
struct QCamera3HdrPlan {
    enum { SETTLE_FRAMES = 2, FRAME_COUNT = 3 * (SETTLE_FRAMES + 1) };
    int32_t exposure[3];

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

// R11 LLHDR consumes four normal exposures and one short exposure. Use the
// verified stock -2 EV variant through ordinary rational AE compensation;
// the -3 EV private bracket exceeds the stock public AE range.
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
