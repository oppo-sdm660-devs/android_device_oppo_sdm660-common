/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_ZSL_RING_POLICY_H
#define QCAMERA3_ZSL_RING_POLICY_H

#include <stdint.h>
#include <vector>

namespace qcamera {

/*
 * Pure policy/stats for the standard-HALv3 ZSL ring (M1 PoC). No Android
 * dependencies so it can be host-tested; the runtime ring lives in
 * QCamera3PicChannel.
 */
class QCamera3ZslRingPolicy {
public:
    static constexpr int32_t kDefaultDepth = 2;
    static constexpr int32_t kMinDepth = 1;
    static constexpr int32_t kMaxDepth = 4;
    static constexpr int64_t kDefaultMaxAgeMs = 120;

    /* One complete ring frame; bufIndex is the HAL snapshot buffer slot. */
    struct Frame {
        uint32_t frameNumber = 0;
        int64_t timestampNs = 0;   /* source sensor timestamp (result metadata) */
        int64_t receivedNs = 0;    /* HAL receipt time, same clock as nowNs */
        uint8_t bufIndex = 0;
        bool inUse = false;
        bool metadataReady = false;
    };

    struct Request {
        bool standardEnableZsl;
        bool stillCapture;
        bool automaticControls;
        bool flashOff;
        bool ordinarySession;
        bool nativeModeOff;
        bool ringEnabled;
    };
    static bool eligible(const Request &request) {
        return request.standardEnableZsl && request.stillCapture &&
                request.automaticControls && request.flashOff && request.ordinarySession &&
                request.nativeModeOff && request.ringEnabled;
    }

    struct Stats {
        uint32_t received = 0;
        uint32_t dropped = 0;      /* callbacks without a usable snapshot buffer */
        uint32_t overwritten = 0;  /* frames replaced before being selected */
        int64_t firstTs = 0;
        int64_t lastTs = 0;
        int64_t tsMin = 0;
        int64_t tsMax = 0;
        int64_t tsSum = 0;
        uint64_t intervalSumNs = 0;
        uint32_t intervalCount = 0;
        uint32_t maxGapMs = 0;
        void noteFrame(int64_t timestampNs);
        void noteDropped();
        void noteOverwritten();
        int64_t averageTimestamp() const;
        /* fps*100 over observed frame intervals (0 when <2 frames). */
        uint32_t averageFpsX100() const;
        int32_t maxGapMsValue() const { return int32_t(maxGapMs); }
    };

    struct Selection {
        bool selected = false;
        Frame frame;
        int32_t reason = kFallbackNoFrame;
    };

    enum Reason {
        kSelected = 0,
        kFallbackNoFrame,
        kFallbackTooOld,
        kFallbackFutureOnly,
        kFallbackFrameInUse,
        kFallbackMetadataMissing,
    };

    static int32_t clampDepth(int32_t depth);
    /* Snapshot buffers a ring of the given depth needs (ring + one inflight). */
    static int32_t ringBufferCount(int32_t depth);
    /* NV21/YCbCr snapshot bytes: w*h*3/2 per frame. */
    static int64_t estimateRingBytes(int32_t width, int32_t height, int32_t buffers);

    /*
     * Pick the newest complete frame with timestampNs <= requestTimestampNs and
     * age <= maxAgeMs. Age uses receivedNs (same clock as nowNs) when present,
     * else timestampNs; this keeps selection correct even when the sensor
     * timestamp time base differs from the HAL clock. Skips in-use frames.
     * Ring may contain out-of-order or duplicate timestamps; later entries win
     * on equal timestamps. Returns Selection.selected=false with a Reason on
     * fallback.
     */
    static Selection select(const std::vector<Frame> &ring,
            int64_t requestTimestampNs, int64_t nowNs, int64_t maxAgeMs);
    static const char *reasonName(int32_t reason);
};

}  // namespace qcamera

#endif /* QCAMERA3_ZSL_RING_POLICY_H */
