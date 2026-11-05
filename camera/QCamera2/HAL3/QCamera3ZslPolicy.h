/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_ZSL_POLICY_H
#define QCAMERA3_ZSL_POLICY_H

#include <stdint.h>

namespace qcamera {

/*
 * Pure decision logic for the vendor-only ZSL path. The HAL3 plumbing reuses the
 * stock HAL ZSL snapshot channel (daemon-maintained ring, pre-allocated snapshot
 * buffers); this class only decides whether a capture request may enter it, and
 * how the selected past frame is reported.
 *
 * Everything here is host-testable and independent of Android/Camera3 headers.
 */
class QCamera3ZslPolicy {
public:
    /* Result values published as org.lineageos.camera.zsl.zslStatus. */
    enum Status : int32_t {
        kStatusPending = 0,   /* request entered ZSL, selection not finished yet */
        kStatusUsed = 1,      /* past frame inside the age budget */
        kStatusStale = 2,     /* past frame older than the budget, still delivered */
        kStatusFallback = 3,  /* request did not use ZSL; normal capture delivered */
        kStatusError = 4,     /* ZSL path failed; request failed or fell back */
    };

    enum class Reason : int32_t {
        kOk = 0,
        kPropertyDisabled,
        kNotRequested,
        kStandardZslMissing,
        kNotStillCapture,
        kNotBlob,
        kVideo,
        kNativeMode,
        kInputReprocess,
        kPairedOrAux,
        kSecure,
        kFlash,
        kAeOff,
        kAeLock,
        kAwbOff,
        kAfOff,
        kRingUnavailable,
        kMemoryLimit,
    };

    /* Published as org.lineageos.camera.zsl.fallbackReason. */
    enum FallbackReason : int32_t {
        kFallbackNone = 0,
        kFallbackNoFrame = 1,      /* ring empty / frame not matched */
        kFallbackAge = 2,          /* past frame older than the age budget */
        kFallbackUnsupported = 3,  /* request/session mode excluded */
        kFallbackChannelError = 4, /* ZSL channel drop/timeout error */
    };

    struct RequestFeatures {
        bool zslEnable = false;      /* vendor marker org.lineageos...zslEnable == 1 */
        bool standardEnableZsl = false; /* ANDROID_CONTROL_ENABLE_ZSL == 1 (baseline) */
        int32_t maxAgeMs = 0;        /* vendor key override; 0 = config default */
        bool stillCapture = false;   /* ANDROID_CONTROL_CAPTURE_INTENT_STILL_CAPTURE */
        bool blobRequest = false;    /* request has a JPEG BLOB output */
        bool video = false;          /* HAL video session / video snapshot */
        int32_t nativeStillMode = 0; /* org.lineageos stillMode; 0 = OFF */
        bool inputReprocess = false; /* framework reprocess / input buffer */
        bool pairedOrAux = false;    /* logical/paired/aux camera session */
        bool secure = false;
        bool flashActive = false;    /* flash needed or flash mode != OFF */
        bool aeOff = false;
        bool aeLock = false;
        bool awbOff = false;
        bool afOff = false;
        bool ringAvailable = false;  /* ZSL channel configured and streaming */
    };

    struct Config {
        bool enabled = false;        /* persist.vendor.camera.oppo.zsl.enable */
        int32_t depth = 2;           /* persist...zsl.depth */
        int32_t maxAgeMs = 120;      /* persist...zsl.max_age_ms */
        int64_t maxBytes = 128LL * 1024 * 1024; /* persist...zsl.max_bytes */
        int32_t fallback = 1;        /* persist...zsl.fallback */
        static bool valid(const Config &config);
    };

    struct Decision {
        bool requested = false;   /* vendor trigger present and property enabled */
        bool useZsl = false;      /* enter the stock HAL ZSL capture path */
        bool fallback = false;    /* requested but not entering: live capture */
        bool failRequest = false; /* requested, not entering, fallback disabled */
        int32_t status = kStatusFallback;
        int32_t maxAgeMs = 0;
        int32_t fallbackReason = kFallbackUnsupported;
        Reason reason = Reason::kNotRequested;
    };

    /* Selection-side result of a delivered past frame. */
    struct Selection {
        bool applied = false;      /* zslApplied */
        int32_t frameAgeMs = -1;   /* zslFrameAgeMs, -1 when no frame selected */
        int32_t fallbackReason = kFallbackNoFrame;
        int32_t status = kStatusFallback; /* internal only */
    };

    static constexpr int32_t kMinDepth = 1;
    static constexpr int32_t kMaxDepth = 4;
    static constexpr int32_t kMinMaxAgeMs = 50;
    static constexpr int32_t kMaxMaxAgeMs = 5000;
    static constexpr int32_t kDefaultMaxAgeMs = 120;
    static constexpr int64_t kDefaultMaxBytes = 128LL * 1024 * 1024;
    static constexpr int32_t kMinBuffers = 3;
    static constexpr int32_t kMaxBuffers = 6;

    static Decision decide(const RequestFeatures &features, const Config &config);
    /* Selection-side classification for a past frame of the given age. */
    static int32_t classifyAge(int64_t ageMs, int32_t maxAgeMs);
    static Selection classifySelection(int64_t ageMs, int32_t maxAgeMs);
    static int32_t fallbackReasonFor(Reason reason);
    static int32_t clampDepth(int32_t depth);
    static int32_t clampMaxAgeMs(int32_t maxAgeMs);
    static int32_t clampFallback(int32_t fallback);
    /* Snapshot buffers needed for a ring of the given depth (geometry margin). */
    static int32_t ringBufferCount(int32_t depth);
    /* w*h*3/2 per frame (NV21/YCbCr snapshot), buffers included. */
    static int64_t estimateRingBytes(int32_t width, int32_t height, int32_t buffers);
    static bool fitsMemory(int32_t width, int32_t height, int32_t buffers,
            int64_t maxBytes);
    static const char *reasonName(Reason reason);
};

}  // namespace qcamera

#endif /* QCAMERA3_ZSL_POLICY_H */
