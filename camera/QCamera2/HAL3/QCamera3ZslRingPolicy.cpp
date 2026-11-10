/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3ZslRingPolicy.h"

namespace qcamera {

void QCamera3ZslRingPolicy::Stats::noteFrame(int64_t timestampNs)
{
    if (received == 0) {
        firstTs = timestampNs;
        tsMin = timestampNs;
        tsMax = timestampNs;
    } else {
        if (timestampNs < tsMin) tsMin = timestampNs;
        if (timestampNs > tsMax) tsMax = timestampNs;
        if (lastTs > 0 && timestampNs > lastTs) {
            const uint64_t gapNs = uint64_t(timestampNs - lastTs);
            intervalSumNs += gapNs;
            ++intervalCount;
            const uint32_t gapMs = uint32_t(gapNs / 1000000ULL);
            if (gapMs > maxGapMs) maxGapMs = gapMs;
        }
    }
    lastTs = timestampNs;
    tsSum += timestampNs;
    ++received;
}

void QCamera3ZslRingPolicy::Stats::noteDropped()
{
    ++dropped;
}

void QCamera3ZslRingPolicy::Stats::noteOverwritten()
{
    ++overwritten;
}

int64_t QCamera3ZslRingPolicy::Stats::averageTimestamp() const
{
    return received == 0 ? 0 : tsSum / int64_t(received);
}

uint32_t QCamera3ZslRingPolicy::Stats::averageFpsX100() const
{
    if (intervalCount == 0 || intervalSumNs == 0) return 0;
    const uint64_t avgIntervalNs = intervalSumNs / intervalCount;
    if (avgIntervalNs == 0) return 0;
    return uint32_t(100ULL * 1000000000ULL / avgIntervalNs);
}

int32_t QCamera3ZslRingPolicy::clampDepth(int32_t depth)
{
    if (depth < kMinDepth) return kMinDepth;
    if (depth > kMaxDepth) return kMaxDepth;
    return depth;
}

int32_t QCamera3ZslRingPolicy::ringBufferCount(int32_t depth)
{
    return clampDepth(depth) + 1;
}

int64_t QCamera3ZslRingPolicy::estimateRingBytes(int32_t width, int32_t height,
        int32_t buffers)
{
    if (width <= 0 || height <= 0 || buffers <= 0) return 0;
    return int64_t(width) * int64_t(height) * 3 / 2 * int64_t(buffers);
}

QCamera3ZslRingPolicy::Selection QCamera3ZslRingPolicy::select(
        const std::vector<Frame> &ring, int64_t requestTimestampNs,
        int64_t nowNs, int64_t maxAgeMs)
{
    Selection selection;
    bool sawFuture = false;
    bool sawInUse = false;
    bool sawComplete = false;
    for (size_t i = 0; i < ring.size(); ++i) {
        const Frame &frame = ring[i];
        if (!frame.metadataReady) continue;
        sawComplete = true;
        if (frame.timestampNs > requestTimestampNs) {
            sawFuture = true;
            continue;
        }
        if (frame.inUse) {
            sawInUse = true;
            continue;
        }
        const int64_t ageBaseNs = frame.receivedNs > 0 ? frame.receivedNs : frame.timestampNs;
        if (nowNs > ageBaseNs && (nowNs - ageBaseNs) / 1000000LL > maxAgeMs) {
            continue;
        }
        if (!selection.selected ||
                frame.timestampNs >= selection.frame.timestampNs) {
            selection.selected = true;
            selection.frame = frame;
            selection.reason = kSelected;
        }
    }
    if (!selection.selected) {
        if (!ring.empty() && !sawComplete) {
            selection.reason = kFallbackMetadataMissing;
        } else if (sawInUse) {
            selection.reason = kFallbackFrameInUse;
        } else if (sawFuture) {
            selection.reason = kFallbackFutureOnly;
        } else if (!ring.empty()) {
            selection.reason = kFallbackTooOld;
        } else {
            selection.reason = kFallbackNoFrame;
        }
    }
    return selection;
}

const char *QCamera3ZslRingPolicy::reasonName(int32_t reason)
{
    switch (reason) {
        case kSelected: return "selected";
        case kFallbackNoFrame: return "no_frame";
        case kFallbackTooOld: return "too_old";
        case kFallbackFutureOnly: return "future_only";
        case kFallbackFrameInUse: return "frame_in_use";
        case kFallbackMetadataMissing: return "metadata_missing";
        default: return "unknown";
    }
}

}  // namespace qcamera
