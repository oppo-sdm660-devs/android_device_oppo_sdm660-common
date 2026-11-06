/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3ZslPolicy.h"

namespace qcamera {

namespace {
constexpr int64_t kBytesPerPixelNum = 3;
constexpr int64_t kBytesPerPixelDen = 2;
}  // namespace

bool QCamera3ZslPolicy::Config::valid(const Config &config)
{
    if (config.depth < kMinDepth || config.depth > kMaxDepth) return false;
    if (config.maxAgeMs < kMinMaxAgeMs || config.maxAgeMs > kMaxMaxAgeMs) return false;
    if (config.maxBytes <= 0) return false;
    if (config.fallback != 0 && config.fallback != 1) return false;
    return true;
}

int32_t QCamera3ZslPolicy::clampDepth(int32_t depth)
{
    if (depth < kMinDepth) return kMinDepth;
    if (depth > kMaxDepth) return kMaxDepth;
    return depth;
}

int32_t QCamera3ZslPolicy::clampMaxAgeMs(int32_t maxAgeMs)
{
    if (maxAgeMs <= 0) return kDefaultMaxAgeMs;
    if (maxAgeMs < kMinMaxAgeMs) return kMinMaxAgeMs;
    if (maxAgeMs > kMaxMaxAgeMs) return kMaxMaxAgeMs;
    return maxAgeMs;
}

int32_t QCamera3ZslPolicy::clampFallback(int32_t fallback)
{
    return fallback == 0 ? 0 : 1;
}

int32_t QCamera3ZslPolicy::ringBufferCount(int32_t depth)
{
    int32_t buffers = clampDepth(depth) + 2;  /* ring + inflight margin */
    if (buffers < kMinBuffers) buffers = kMinBuffers;
    if (buffers > kMaxBuffers) buffers = kMaxBuffers;
    return buffers;
}

int64_t QCamera3ZslPolicy::estimateRingBytes(int32_t width, int32_t height,
        int32_t buffers)
{
    if (width <= 0 || height <= 0 || buffers <= 0) return 0;
    const int64_t bytesPerFrame = int64_t(width) * int64_t(height) *
            kBytesPerPixelNum / kBytesPerPixelDen;
    return bytesPerFrame * int64_t(buffers);
}

bool QCamera3ZslPolicy::fitsMemory(int32_t width, int32_t height, int32_t buffers,
        int64_t maxBytes)
{
    if (maxBytes <= 0 || width <= 0 || height <= 0 || buffers <= 0) return false;
    return estimateRingBytes(width, height, buffers) <= maxBytes;
}

QCamera3ZslPolicy::Decision QCamera3ZslPolicy::decide(
        const RequestFeatures &features, const Config &config)
{
    Decision decision;
    if (!features.zslEnable) {
        decision.reason = Reason::kNotRequested;
        return decision;
    }
    /* The vendor keys are only advertised while the master property is on; a
     * request that still sets them while disabled is ignored (no result keys),
     * so a client cannot observe that a hidden path exists. */
    if (!config.enabled || !Config::valid(config)) {
        decision.reason = Reason::kPropertyDisabled;
        return decision;
    }
    decision.requested = true;
    decision.maxAgeMs = features.maxAgeMs > 0 ? clampMaxAgeMs(features.maxAgeMs)
                                            : config.maxAgeMs;
    if (!features.standardEnableZsl) {
        decision.useZsl = false;
        decision.fallback = true;
        decision.fallbackReason = kFallbackUnsupported;
        decision.status = kStatusFallback;
        decision.reason = Reason::kStandardZslMissing;
        if (config.fallback == 0) {
            decision.failRequest = true;
            decision.fallback = false;
            decision.status = kStatusError;
        }
        return decision;
    }
    const auto fallback = [&](Reason reason, bool fail) {
        decision.useZsl = false;
        decision.reason = reason;
        decision.fallbackReason = fallbackReasonFor(reason);
        if (fail && config.fallback == 0) {
            decision.failRequest = true;
            decision.fallback = false;
            decision.status = kStatusError;
        } else {
            decision.fallback = true;
            decision.status = kStatusFallback;
        }
        return decision;
    };

    if (!features.stillCapture) return fallback(Reason::kNotStillCapture, true);
    if (!features.blobRequest) return fallback(Reason::kNotBlob, true);
    if (features.video) return fallback(Reason::kVideo, true);
    if (features.nativeStillMode != 0) return fallback(Reason::kNativeMode, true);
    if (features.inputReprocess) return fallback(Reason::kInputReprocess, true);
    if (features.pairedOrAux) return fallback(Reason::kPairedOrAux, true);
    if (features.secure) return fallback(Reason::kSecure, true);
    if (features.flashActive) return fallback(Reason::kFlash, true);
    if (features.aeOff) return fallback(Reason::kAeOff, true);
    if (features.aeLock) return fallback(Reason::kAeLock, true);
    if (features.awbOff) return fallback(Reason::kAwbOff, true);
    if (features.afOff) return fallback(Reason::kAfOff, true);
    if (!features.ringAvailable) return fallback(Reason::kRingUnavailable, true);

    decision.useZsl = true;
    decision.reason = Reason::kOk;
    decision.status = kStatusPending;
    decision.fallbackReason = kFallbackNone;
    return decision;
}

int32_t QCamera3ZslPolicy::classifyAge(int64_t ageMs, int32_t maxAgeMs)
{
    if (ageMs < 0) return kStatusStale;
    return ageMs <= int64_t(maxAgeMs) ? kStatusUsed : kStatusStale;
}

QCamera3ZslPolicy::Selection QCamera3ZslPolicy::classifySelection(int64_t ageMs,
        int32_t maxAgeMs)
{
    Selection selection;
    if (ageMs < 0) {
        // No usable past frame: normal capture path / channel error.
        selection.applied = false;
        selection.frameAgeMs = -1;
        selection.fallbackReason = kFallbackNoFrame;
        selection.status = kStatusFallback;
        return selection;
    }
    selection.frameAgeMs = int32_t(ageMs > 0x7fffffff ? 0x7fffffff : ageMs);
    if (ageMs <= int64_t(maxAgeMs)) {
        selection.applied = true;
        selection.fallbackReason = kFallbackNone;
        selection.status = kStatusUsed;
    } else {
        // Older than the budget: deliver the frame but report applied=0/age so
        // the client sees a stale ZSL result instead of losing the shutter.
        selection.applied = false;
        selection.fallbackReason = kFallbackAge;
        selection.status = kStatusStale;
    }
    return selection;
}

int32_t QCamera3ZslPolicy::fallbackReasonFor(Reason reason)
{
    switch (reason) {
    case Reason::kOk:
    case Reason::kNotRequested:
        return kFallbackNone;
    case Reason::kRingUnavailable:
        return kFallbackNoFrame;
    case Reason::kPropertyDisabled:
    case Reason::kStandardZslMissing:
    case Reason::kNotStillCapture:
    case Reason::kNotBlob:
    case Reason::kVideo:
    case Reason::kNativeMode:
    case Reason::kInputReprocess:
    case Reason::kPairedOrAux:
    case Reason::kSecure:
    case Reason::kFlash:
    case Reason::kAeOff:
    case Reason::kAeLock:
    case Reason::kAwbOff:
    case Reason::kAfOff:
    case Reason::kMemoryLimit:
        return kFallbackUnsupported;
    }
    return kFallbackUnsupported;
}

bool QCamera3ZslPolicy::suppressHalZslBlobDrop(bool isBlobStream,
        bool isZslChannel, bool bypassEnabled)
{
    if (!bypassEnabled) return false;
    return isBlobStream && isZslChannel;
}

bool QCamera3ZslPolicy::suppressHalZslPseudoFrameDrop(bool pastFrameRequest,
        bool bypassEnabled)
{
    return bypassEnabled && !pastFrameRequest;
}

const char *QCamera3ZslPolicy::reasonName(Reason reason)
{
    switch (reason) {
    case Reason::kOk: return "ok";
    case Reason::kPropertyDisabled: return "property-disabled";
    case Reason::kStandardZslMissing: return "standard-zsl-missing";
    case Reason::kNotRequested: return "not-requested";
    case Reason::kNotStillCapture: return "not-still-capture";
    case Reason::kNotBlob: return "not-blob";
    case Reason::kVideo: return "video";
    case Reason::kNativeMode: return "native-mode";
    case Reason::kInputReprocess: return "input-reprocess";
    case Reason::kPairedOrAux: return "paired-or-aux";
    case Reason::kSecure: return "secure";
    case Reason::kFlash: return "flash";
    case Reason::kAeOff: return "ae-off";
    case Reason::kAeLock: return "ae-lock";
    case Reason::kAwbOff: return "awb-off";
    case Reason::kAfOff: return "af-off";
    case Reason::kRingUnavailable: return "ring-unavailable";
    case Reason::kMemoryLimit: return "memory-limit";
    }
    return "unknown";
}

}  // namespace qcamera
