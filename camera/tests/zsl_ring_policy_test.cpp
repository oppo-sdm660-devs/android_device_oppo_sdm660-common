/* SPDX-License-Identifier: BSD-3-Clause */
/* Host tests for the standard-ZSL HAL ring policy (pure logic, task-113 M1). */
#include "QCamera3ZslRingPolicy.h"

#include <stdio.h>

using qcamera::QCamera3ZslRingPolicy;
using Frame = QCamera3ZslRingPolicy::Frame;
using Stats = QCamera3ZslRingPolicy::Stats;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

namespace {

constexpr int64_t kMs = 1000000LL;

Frame frame(uint32_t number, int64_t tsMs, uint8_t buf, bool inUse = false)
{
    Frame f;
    f.frameNumber = number;
    f.timestampNs = tsMs * kMs;
    f.bufIndex = buf;
    f.inUse = inUse;
    return f;
}

Frame frameWithReceipt(uint32_t number, int64_t tsMs, int64_t receivedMs,
        uint8_t buf, bool inUse = false)
{
    Frame f = frame(number, tsMs, buf, inUse);
    f.receivedNs = receivedMs * kMs;
    return f;
}

void testSizing()
{
    CHECK(QCamera3ZslRingPolicy::clampDepth(0) == 1);
    CHECK(QCamera3ZslRingPolicy::clampDepth(9) == 4);
    CHECK(QCamera3ZslRingPolicy::clampDepth(2) == 2);
    CHECK(QCamera3ZslRingPolicy::ringBufferCount(1) == 2);
    CHECK(QCamera3ZslRingPolicy::ringBufferCount(4) == 5);
    CHECK(QCamera3ZslRingPolicy::estimateRingBytes(4608, 3456, 4) ==
            int64_t(4608) * 3456 * 3 / 2 * 4);
    CHECK(QCamera3ZslRingPolicy::estimateRingBytes(0, 100, 4) == 0);
    CHECK(QCamera3ZslRingPolicy::estimateRingBytes(100, 100, 0) == 0);
}

void testSelection()
{
    std::vector<Frame> ring;
    // Empty ring.
    auto s = QCamera3ZslRingPolicy::select(ring, 1000 * kMs, 1000 * kMs, 120);
    CHECK(!s.selected);
    CHECK(s.reason == QCamera3ZslRingPolicy::kFallbackNoFrame);

    // Newest at-or-before the request timestamp wins.
    ring.push_back(frame(1, 900, 0));
    ring.push_back(frame(2, 950, 1));
    ring.push_back(frame(3, 990, 2));
    s = QCamera3ZslRingPolicy::select(ring, 1000 * kMs, 1000 * kMs, 120);
    CHECK(s.selected);
    CHECK(s.frame.frameNumber == 3);
    CHECK(s.frame.bufIndex == 2);
    CHECK(s.frame.timestampNs == 990 * kMs);

    // Request older than every frame -> future only.
    s = QCamera3ZslRingPolicy::select(ring, 800 * kMs, 1000 * kMs, 120);
    CHECK(!s.selected);
    CHECK(s.reason == QCamera3ZslRingPolicy::kFallbackFutureOnly);

    // Out-of-order ring: newest eligible by timestamp, not by index.
    ring.clear();
    ring.push_back(frame(10, 980, 0));
    ring.push_back(frame(11, 950, 1));
    ring.push_back(frame(12, 990, 2));
    s = QCamera3ZslRingPolicy::select(ring, 985 * kMs, 1000 * kMs, 120);
    CHECK(s.selected && s.frame.frameNumber == 10);

    // Equal timestamps: later entry wins.
    ring.clear();
    ring.push_back(frame(20, 970, 0));
    ring.push_back(frame(21, 970, 1));
    s = QCamera3ZslRingPolicy::select(ring, 980 * kMs, 1000 * kMs, 120);
    CHECK(s.selected && s.frame.frameNumber == 21);

    // In-use frame is not selected; older eligible frame is.
    ring.clear();
    ring.push_back(frame(30, 960, 0));
    ring.push_back(frame(31, 990, 1, true));
    s = QCamera3ZslRingPolicy::select(ring, 1000 * kMs, 1000 * kMs, 120);
    CHECK(s.selected && s.frame.frameNumber == 30);

    // All frames in use -> explicit fallback reason.
    ring.clear();
    ring.push_back(frame(40, 970, 0, true));
    s = QCamera3ZslRingPolicy::select(ring, 1000 * kMs, 1000 * kMs, 120);
    CHECK(!s.selected);
    CHECK(s.reason == QCamera3ZslRingPolicy::kFallbackFrameInUse);

    // Age limit: latest eligible frame is too old.
    ring.clear();
    ring.push_back(frame(50, 500, 0));
    s = QCamera3ZslRingPolicy::select(ring, 1000 * kMs, 1000 * kMs, 120);
    CHECK(!s.selected);
    CHECK(s.reason == QCamera3ZslRingPolicy::kFallbackTooOld);

    // Receipt-time aging: a frame whose sensor timestamp is in a different time
    // base is still selectable when it was received recently (ring restart case).
    ring.clear();
    ring.push_back(frameWithReceipt(60, 1000, 1000, 0));
    ring.push_back(frameWithReceipt(61, 1001, 1010, 1));
    s = QCamera3ZslRingPolicy::select(ring, 2000 * kMs, 1020 * kMs, 300);
    CHECK(s.selected && s.frame.frameNumber == 61);
    CHECK(s.frame.receivedNs == 1010 * kMs);

    // Receipt age > maxAge still falls back even if sensor ts looks recent.
    ring.clear();
    ring.push_back(frameWithReceipt(70, 1001, 500, 0));
    s = QCamera3ZslRingPolicy::select(ring, 2000 * kMs, 1020 * kMs, 300);
    CHECK(!s.selected);
    CHECK(s.reason == QCamera3ZslRingPolicy::kFallbackTooOld);

    CHECK(QCamera3ZslRingPolicy::reasonName(QCamera3ZslRingPolicy::kSelected) != nullptr);
    CHECK(QCamera3ZslRingPolicy::reasonName(12345) != nullptr);
}

void testStats()
{
    Stats stats;
    CHECK(stats.received == 0);
    CHECK(stats.averageFpsX100() == 0);
    CHECK(stats.averageTimestamp() == 0);

    // 30fps stream: 10 frames 33.33ms apart, timestamps 1000..1300ms.
    for (int i = 0; i < 10; ++i) {
        stats.noteFrame((1000 + i * 33) * kMs);
    }
    CHECK(stats.received == 10);
    CHECK(stats.firstTs == 1000 * kMs);
    CHECK(stats.lastTs == 1297 * kMs);
    CHECK(stats.tsMin == 1000 * kMs);
    CHECK(stats.tsMax == 1297 * kMs);
    CHECK(stats.intervalCount == 9);
    CHECK(stats.maxGapMs == 33);
    CHECK(stats.averageTimestamp() == (1000 * kMs + 1297 * kMs) / 2);
    const uint32_t fpsX100 = stats.averageFpsX100();
    CHECK(fpsX100 >= 2999 && fpsX100 <= 3031);  // 30.0fps around 33ms

    // Out-of-order timestamp must not corrupt interval stats (monotonic gaps only).
    stats.noteFrame(900 * kMs);
    CHECK(stats.received == 11);
    CHECK(stats.tsMin == 900 * kMs);
    CHECK(stats.lastTs == 900 * kMs);

    stats.noteDropped();
    stats.noteDropped();
    stats.noteOverwritten();
    CHECK(stats.dropped == 2);
    CHECK(stats.overwritten == 1);

    // Large gap is tracked for the M1 health log.
    stats.noteFrame(3000 * kMs);
    CHECK(stats.maxGapMs == 2100);
}

}  // namespace

int main()
{
    testSizing();
    testSelection();
    testStats();
    if (g_failures) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("zsl ring policy host tests: all checks passed\n");
    return 0;
}
