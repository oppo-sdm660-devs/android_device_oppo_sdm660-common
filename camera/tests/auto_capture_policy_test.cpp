/* SPDX-License-Identifier: BSD-3-Clause */
// Host behavior test for the automatic HDR/night policy and the luma histogram
// helpers used by the HAL3 metadata path (no Android dependency).
#include "QCamera3AutoCapturePolicy.h"

#include <stdio.h>
#include <string.h>

using qcamera::QCamera3AutoCapturePolicy;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

namespace {

void fillHistogram(uint32_t *histogram, const uint32_t (&bins)[256], uint32_t &total) {
    total = 0;
    for (int i = 0; i < 256; ++i) {
        histogram[i] = bins[i];
        total += bins[i];
    }
}

void testConfigValidation() {
    QCamera3AutoCapturePolicy::Config config;
    CHECK(QCamera3AutoCapturePolicy::Config::valid(config));
    config.nightEnterIndex = 0;
    CHECK(!QCamera3AutoCapturePolicy::Config::valid(config));
    config = QCamera3AutoCapturePolicy::Config();
    config.hdrEnable = 2;
    CHECK(!QCamera3AutoCapturePolicy::Config::valid(config));
}

void testHistogramClipping() {
    uint32_t histogram[256] = {};
    // 1000 px: 100 at 255 (10%), 50 at 0 (5%), 850 at 128.
    histogram[255] = 100;
    histogram[0] = 50;
    histogram[128] = 850;
    int32_t highlight = -1, shadow = -1;
    CHECK(QCamera3AutoCapturePolicy::histogramClipping(histogram, 1000, highlight, shadow));
    CHECK(highlight == 100);
    CHECK(shadow == 50);
    // An inconsistent total is refused instead of guessing.
    CHECK(!QCamera3AutoCapturePolicy::histogramClipping(histogram, 999, highlight, shadow));
    CHECK(!QCamera3AutoCapturePolicy::histogramClipping(nullptr, 0, highlight, shadow));
}

void testMissingStats() {
    QCamera3AutoCapturePolicy policy;
    QCamera3AutoCapturePolicy::Config config;
    QCamera3AutoCapturePolicy::Stats stats;
    const auto decision = policy.decide(stats, config, 10);
    CHECK(!decision.triggers());
    CHECK(decision.reason == QCamera3AutoCapturePolicy::kReasonInvalidStats ||
          decision.reason == QCamera3AutoCapturePolicy::kReasonMissingStats);
}

void testLowLightTrigger() {
    QCamera3AutoCapturePolicy policy;
    QCamera3AutoCapturePolicy::Config config;
    config.hdrEnable = 1;
    config.nightEnable = 1;
    QCamera3AutoCapturePolicy::Stats stats;
    stats.exposureTimeNs = 33333333;  // 33 ms preview exposure
    stats.iso = 1600;                 // darkness index = 33 * 1600 / 10 = 5280
    CHECK(QCamera3AutoCapturePolicy::validStats(stats));
    const auto first = policy.decide(stats, config, 100);
    const auto second = policy.decide(stats, config, 101);
    const auto third = policy.decide(stats, config, 102);
    CHECK(!first.triggers());
    CHECK(!second.triggers());
    CHECK(third.triggers());
    CHECK(third.mode == QCamera3AutoCapturePolicy::kModeNight);
    CHECK(third.reason == QCamera3AutoCapturePolicy::kReasonLowLight);
}

void testHdrClippingTrigger() {
    QCamera3AutoCapturePolicy policy;
    QCamera3AutoCapturePolicy::Config config;
    config.hdrEnable = 1;
    config.nightEnable = 0;
    QCamera3AutoCapturePolicy::Stats stats;
    stats.exposureTimeNs = 10000000;
    stats.iso = 100;
    stats.highlightClippedMilli = 120;  // 12 % saturated: above the 5 % enter level
    CHECK(QCamera3AutoCapturePolicy::validStats(stats));
    policy.decide(stats, config, 1);
    policy.decide(stats, config, 2);
    const auto decision = policy.decide(stats, config, 3);
    CHECK(decision.triggers());
    CHECK(decision.mode == QCamera3AutoCapturePolicy::kModeHdr);
    CHECK(decision.reason == QCamera3AutoCapturePolicy::kReasonBacklight);
}

void testHdrDynamicRangeReason() {
    QCamera3AutoCapturePolicy policy;
    QCamera3AutoCapturePolicy::Config config;
    config.hdrEnable = 1;
    config.nightEnable = 0;
    uint32_t bins[256] = {};
    // Symmetric wide distribution: p5=20, p50~120/140, p95=240, little clipping.
    bins[20] = 250;
    bins[120] = 250;
    bins[140] = 250;
    bins[240] = 250;
    uint32_t histogram[256] = {};
    uint32_t total = 0;
    fillHistogram(histogram, bins, total);
    QCamera3AutoCapturePolicy::Stats stats;
    stats.lumaHistogram = histogram;
    stats.histogramTotal = total;
    CHECK(QCamera3AutoCapturePolicy::validStats(stats));
    policy.decide(stats, config, 1);
    policy.decide(stats, config, 2);
    const auto decision = policy.decide(stats, config, 3);
    CHECK(decision.triggers());
    CHECK(decision.mode == QCamera3AutoCapturePolicy::kModeHdr);
    CHECK(decision.reason == QCamera3AutoCapturePolicy::kReasonDynamicRange);
}

void testMotionBlocks() {
    QCamera3AutoCapturePolicy policy;
    QCamera3AutoCapturePolicy::Config config;
    QCamera3AutoCapturePolicy::Stats stats;
    stats.exposureTimeNs = 33333333;
    stats.iso = 1600;
    stats.motionMilli = 800;
    policy.decide(stats, config, 1);
    policy.decide(stats, config, 2);
    const auto decision = policy.decide(stats, config, 3);
    CHECK(!decision.triggers());
    CHECK(decision.reason == QCamera3AutoCapturePolicy::kReasonMotion);
}

}  // namespace

int main() {
    testConfigValidation();
    testHistogramClipping();
    testMissingStats();
    testLowLightTrigger();
    testHdrClippingTrigger();
    testHdrDynamicRangeReason();
    testMotionBlocks();
    if (g_failures) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("auto capture policy host tests: all checks passed\n");
    return 0;
}
