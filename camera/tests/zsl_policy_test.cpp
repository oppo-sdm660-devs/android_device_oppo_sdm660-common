/* SPDX-License-Identifier: BSD-3-Clause */
/* Host tests for the vendor-only ZSL request/selection policy (pure logic). */
#include "QCamera3ZslPolicy.h"

#include <stdio.h>
#include <string.h>

using qcamera::QCamera3ZslPolicy;
using Features = QCamera3ZslPolicy::RequestFeatures;
using Config = QCamera3ZslPolicy::Config;
using Decision = QCamera3ZslPolicy::Decision;
using Reason = QCamera3ZslPolicy::Reason;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

namespace {

Config goodConfig()
{
    Config config;
    config.enabled = true;
    config.depth = 2;
    config.maxAgeMs = 120;
    config.maxBytes = QCamera3ZslPolicy::kDefaultMaxBytes;
    config.fallback = 1;
    return config;
}

Features goodFeatures()
{
    Features features;
    features.zslEnable = true;
    features.standardEnableZsl = true;
    features.stillCapture = true;
    features.blobRequest = true;
    features.ringAvailable = true;
    return features;
}

void testConfig() {
    Config config;
    CHECK(QCamera3ZslPolicy::Config::valid(config));
    config.depth = 0;
    CHECK(!QCamera3ZslPolicy::Config::valid(config));
    config = goodConfig();
    config.maxAgeMs = 20;
    CHECK(!QCamera3ZslPolicy::Config::valid(config));
    config = goodConfig();
    config.fallback = 2;
    CHECK(!QCamera3ZslPolicy::Config::valid(config));
    CHECK(QCamera3ZslPolicy::clampDepth(-1) == 1);
    CHECK(QCamera3ZslPolicy::clampDepth(2) == 2);
    CHECK(QCamera3ZslPolicy::clampDepth(9) == 4);
    CHECK(QCamera3ZslPolicy::clampMaxAgeMs(0) == 120);
    CHECK(QCamera3ZslPolicy::clampMaxAgeMs(10) == 50);
    CHECK(QCamera3ZslPolicy::clampMaxAgeMs(9000) == 5000);
    CHECK(QCamera3ZslPolicy::clampFallback(7) == 1);
    CHECK(QCamera3ZslPolicy::clampFallback(0) == 0);
}

void testRingSizing() {
    CHECK(QCamera3ZslPolicy::ringBufferCount(1) == 3);
    CHECK(QCamera3ZslPolicy::ringBufferCount(2) == 4);
    CHECK(QCamera3ZslPolicy::ringBufferCount(3) == 5);
    CHECK(QCamera3ZslPolicy::ringBufferCount(4) == 6);
    CHECK(QCamera3ZslPolicy::ringBufferCount(9) == 6);
    // 16 MP snapshot: 4608x3456x1.5 = 23,887,872 bytes per frame.
    const int64_t perFrame = QCamera3ZslPolicy::estimateRingBytes(4608, 3456, 1);
    CHECK(perFrame == 23887872);
    CHECK(QCamera3ZslPolicy::estimateRingBytes(4608, 3456, 4) == perFrame * 4);
    CHECK(QCamera3ZslPolicy::fitsMemory(4608, 3456, 4,
            QCamera3ZslPolicy::kDefaultMaxBytes));
    // A cap below the estimated ring must be rejected, not silently allocated.
    CHECK(!QCamera3ZslPolicy::fitsMemory(4608, 3456, 4, perFrame * 4 - 1));
    CHECK(!QCamera3ZslPolicy::fitsMemory(0, 3456, 4,
            QCamera3ZslPolicy::kDefaultMaxBytes));
}

void testNotRequested() {
    Features features = goodFeatures();
    features.zslEnable = false;
    const Decision decision = QCamera3ZslPolicy::decide(features, goodConfig());
    CHECK(!decision.requested);
    CHECK(!decision.useZsl);
    CHECK(!decision.fallback);
    CHECK(decision.reason == Reason::kNotRequested);
}

void testPropertyDisabled() {
    Features features = goodFeatures();
    Config config = goodConfig();
    config.enabled = false;
    const Decision decision = QCamera3ZslPolicy::decide(features, config);
    CHECK(!decision.requested);
    CHECK(!decision.useZsl);
    CHECK(decision.reason == Reason::kPropertyDisabled);
}

void testHappyPath() {
    const Decision decision = QCamera3ZslPolicy::decide(goodFeatures(), goodConfig());
    CHECK(decision.requested);
    CHECK(decision.useZsl);
    CHECK(!decision.fallback);
    CHECK(!decision.failRequest);
    CHECK(decision.status == QCamera3ZslPolicy::kStatusPending);
    CHECK(decision.maxAgeMs == 120);
    CHECK(decision.fallbackReason == QCamera3ZslPolicy::kFallbackNone);
    CHECK(decision.reason == Reason::kOk);
}

void testMaxAgeOverride() {
    Features features = goodFeatures();
    features.maxAgeMs = 50;
    Decision decision = QCamera3ZslPolicy::decide(features, goodConfig());
    CHECK(decision.maxAgeMs == 50);
    features.maxAgeMs = 10;   // clamped to 50
    decision = QCamera3ZslPolicy::decide(features, goodConfig());
    CHECK(decision.maxAgeMs == 50);
    features.maxAgeMs = 9000; // clamped to 5000
    decision = QCamera3ZslPolicy::decide(features, goodConfig());
    CHECK(decision.maxAgeMs == 5000);
    features.maxAgeMs = 0;    // config default
    decision = QCamera3ZslPolicy::decide(features, goodConfig());
    CHECK(decision.maxAgeMs == 120);
}

void testStandardKeyRequired() {
    Features features = goodFeatures();
    features.standardEnableZsl = false;
    Decision decision = QCamera3ZslPolicy::decide(features, goodConfig());
    CHECK(decision.requested);
    CHECK(!decision.useZsl);
    CHECK(decision.fallback);
    CHECK(decision.reason == Reason::kStandardZslMissing);
    CHECK(decision.fallbackReason == QCamera3ZslPolicy::kFallbackUnsupported);
}

void testExclusions() {
    struct Case {
        const char *name;
        void (*mutate)(Features &);
        Reason reason;
    };
    const Case cases[] = {
        {"not-still", [](Features &f) { f.stillCapture = false; }, Reason::kNotStillCapture},
        {"not-blob", [](Features &f) { f.blobRequest = false; }, Reason::kNotBlob},
        {"video", [](Features &f) { f.video = true; }, Reason::kVideo},
        {"native", [](Features &f) { f.nativeStillMode = 1; }, Reason::kNativeMode},
        {"reprocess", [](Features &f) { f.inputReprocess = true; }, Reason::kInputReprocess},
        {"paired", [](Features &f) { f.pairedOrAux = true; }, Reason::kPairedOrAux},
        {"secure", [](Features &f) { f.secure = true; }, Reason::kSecure},
        {"flash", [](Features &f) { f.flashActive = true; }, Reason::kFlash},
        {"ae-off", [](Features &f) { f.aeOff = true; }, Reason::kAeOff},
        {"ae-lock", [](Features &f) { f.aeLock = true; }, Reason::kAeLock},
        {"awb-off", [](Features &f) { f.awbOff = true; }, Reason::kAwbOff},
        {"af-off", [](Features &f) { f.afOff = true; }, Reason::kAfOff},
        {"ring", [](Features &f) { f.ringAvailable = false; }, Reason::kRingUnavailable},
    };
    for (const Case &item : cases) {
        Features features = goodFeatures();
        item.mutate(features);
        Decision decision = QCamera3ZslPolicy::decide(features, goodConfig());
        if (!decision.requested || decision.useZsl || !decision.fallback ||
                decision.reason != item.reason) {
            fprintf(stderr, "case %s: requested=%d useZsl=%d fallback=%d reason=%s\n",
                    item.name, decision.requested, decision.useZsl,
                    decision.fallback,
                    QCamera3ZslPolicy::reasonName(decision.reason));
            ++g_failures;
        }
        CHECK(decision.status == QCamera3ZslPolicy::kStatusFallback);
        const int32_t expectedReason = item.reason == Reason::kRingUnavailable ?
                QCamera3ZslPolicy::kFallbackNoFrame : QCamera3ZslPolicy::kFallbackUnsupported;
        CHECK(decision.fallbackReason == expectedReason);
        // With fallback disabled the same request must fail explicitly.
        Config strict = goodConfig();
        strict.fallback = 0;
        decision = QCamera3ZslPolicy::decide(features, strict);
        CHECK(decision.failRequest);
        CHECK(!decision.fallback);
        CHECK(decision.status == QCamera3ZslPolicy::kStatusError);
    }
}

void testAgeClassification() {
    CHECK(QCamera3ZslPolicy::classifyAge(0, 120) == QCamera3ZslPolicy::kStatusUsed);
    CHECK(QCamera3ZslPolicy::classifyAge(120, 120) == QCamera3ZslPolicy::kStatusUsed);
    CHECK(QCamera3ZslPolicy::classifyAge(121, 120) == QCamera3ZslPolicy::kStatusStale);
    CHECK(QCamera3ZslPolicy::classifyAge(-5, 120) == QCamera3ZslPolicy::kStatusStale);
    CHECK(QCamera3ZslPolicy::classifyAge(150, 100) == QCamera3ZslPolicy::kStatusStale);

    auto selection = QCamera3ZslPolicy::classifySelection(80, 120);
    CHECK(selection.applied);
    CHECK(selection.frameAgeMs == 80);
    CHECK(selection.fallbackReason == QCamera3ZslPolicy::kFallbackNone);
    selection = QCamera3ZslPolicy::classifySelection(300, 120);
    CHECK(!selection.applied);
    CHECK(selection.frameAgeMs == 300);
    CHECK(selection.fallbackReason == QCamera3ZslPolicy::kFallbackAge);
    selection = QCamera3ZslPolicy::classifySelection(-1, 120);
    CHECK(!selection.applied);
    CHECK(selection.frameAgeMs == -1);
    CHECK(selection.fallbackReason == QCamera3ZslPolicy::kFallbackNoFrame);
}

void testReasonNames() {
    CHECK(strcmp(QCamera3ZslPolicy::reasonName(Reason::kOk), "ok") == 0);
    CHECK(strcmp(QCamera3ZslPolicy::reasonName(Reason::kNativeMode), "native-mode") == 0);
    CHECK(strcmp(QCamera3ZslPolicy::reasonName(Reason::kRingUnavailable),
            "ring-unavailable") == 0);
    CHECK(strcmp(QCamera3ZslPolicy::reasonName(Reason::kStandardZslMissing),
            "standard-zsl-missing") == 0);
    CHECK(QCamera3ZslPolicy::fallbackReasonFor(Reason::kVideo) ==
            QCamera3ZslPolicy::kFallbackUnsupported);
    CHECK(QCamera3ZslPolicy::fallbackReasonFor(Reason::kRingUnavailable) ==
            QCamera3ZslPolicy::kFallbackNoFrame);
    CHECK(QCamera3ZslPolicy::fallbackReasonFor(Reason::kOk) ==
            QCamera3ZslPolicy::kFallbackNone);
}

}  // namespace

int main() {
    testConfig();
    testRingSizing();
    testNotRequested();
    testPropertyDisabled();
    testHappyPath();
    testMaxAgeOverride();
    testStandardKeyRequired();
    testExclusions();
    testAgeClassification();
    testReasonNames();
    if (g_failures) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("zsl policy host tests: all checks passed\n");
    return 0;
}
