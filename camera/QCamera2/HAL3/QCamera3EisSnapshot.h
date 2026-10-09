/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_EIS_SNAPSHOT_H
#define QCAMERA3_EIS_SNAPSHOT_H

#include "QCamera3MmsVstab.h"

#include <atomic>
#include <stddef.h>
#include <stdint.h>

namespace qcamera {

// EIS-aware still selection: freeze and reuse the gyro-stabilised ZSL frame.
class QCamera3EisSnapshot {
public:
    enum {
        MAX_CANDIDATES = 16,
        MAX_FROZEN_FRAMES = 1,
    };

    // Negotiated stabilisation level: 0 none, 1 DIS, 2 EIS 2.0, 3 EIS 3.0.
    enum EisVersion {
        kEisNone = 0,
        kEisDis = 1,
        kEis2 = 2,
        kEis3 = 3,
    };

    enum Reason {
        kReasonSelectedStabilized = 0,
        kReasonSelectedFallback = 1,
        kReasonSelectedFrozen = 2,
        kReasonEisDisabled = 3,
        kReasonEisMissing = 4,
        kReasonNoStabilizedFrame = 5,
        kReasonStale = 6,
        kReasonMotion = 7,
        kReasonMalformed = 8,
        kReasonFrozenInvalid = 9,
        kReasonNothingFrozen = 10,
    };

    // Filled from the MMS path and ANDROID_CONTROL_VIDEO_STABILIZATION_MODE.
    struct EisState {
        int32_t enabled = 0;
        int32_t configured = 0;
        int32_t coreStatus = 0;
        int32_t gyroValid = 0;
        int64_t gyroPeriodNs = 0;
        int32_t version = kEisNone;
    };

    struct Candidate {
        uint32_t frameNumber = 0;
        int64_t sensorTimestampNs = 0;
        int64_t exposureNs = 0;
        int32_t zsl = 0;
        int32_t eisApplied = 0;
        int32_t coreStatus = 0;
        int32_t gyroSamples = 0;  // samples covering the exposure window
        int32_t motionMilli = -1; // gyro magnitude; -1 = absent
        int32_t cropMilli = 0;    // crop factor * 1000
    };

    struct Options {
        // 0 allows the unstabilised fallback, reported as usedEis 0.
        int32_t requireStabilized = 1;
        int64_t maxAgeNs = 0;
        int64_t nowNs = 0;
        int32_t maxMotionMilli = 700;
        int32_t minGyroSamples = 1;
        int32_t maxCropMilli = 2000;
        uint32_t frameNumber = 0;
        static bool valid(const Options &o);
    };

    struct Selection {
        uint32_t index = 0;
        uint32_t frameNumber = 0;
        int64_t sensorTimestampNs = 0;
        int64_t ageNs = 0;
        int32_t motionMilli = -1;
        int32_t cropMilli = 0;
        int32_t usedEis = 0;
        int32_t reason = kReasonNoStabilizedFrame;
        int32_t considered = 0;
        int32_t rejected = 0;
    };

    struct Frozen {
        int32_t active = 0;
        uint32_t frameNumber = 0;
        uint32_t index = 0;
        int64_t sensorTimestampNs = 0;
        int64_t frozenAtNs = 0;
        int32_t motionMilli = -1;
        int32_t cropMilli = 0;
        int32_t usedEis = 0;
    };

    struct Result {
        int32_t status = 0;    // 0 pass; negative errno otherwise
        int32_t applied = 0;
        uint32_t frameNumber = 0;
        // detail[0] = Reason, detail[1] = candidates considered,
        // detail[2] = candidates rejected, detail[3] = selected age in ms.
        int32_t detail[4] = {};
    };

    static const char *name();
    static bool isAvailable();
    static bool supports(uint32_t width, uint32_t height);
    static bool validOptions(const Options &options);
    static bool validEisState(const EisState &state);
    static bool validCandidate(const Candidate &candidate);

    // Builds a candidate from an MMS result; false leaves it untouched.
    static bool fromMmsResult(const QCamera3MmsVstab::Result &result, int32_t zsl,
            int32_t gyroSamples, int32_t motionMilli, int32_t cropMilli,
            Candidate &candidate);

    // Selection is written only on success; reason in Result::detail[0].
    static int32_t select(const EisState &state, const Candidate *candidates,
            uint32_t count, const Options &options, Result &result, Selection &selection,
            const std::atomic<bool> *cancelled = nullptr);

    // Refuses a selection that is not stabilised.
    int32_t freeze(const Selection &selection, int64_t nowNs, Result &result,
            const std::atomic<bool> *cancelled = nullptr);

    // -ENOENT when nothing is frozen; no fallback to another frame.
    int32_t reuse(uint32_t frameNumber, Result &result, Selection &selection,
            const std::atomic<bool> *cancelled = nullptr) const;

    void release();
    const Frozen &frozen() const { return mFrozen; }

    void reset();

private:
    Frozen mFrozen;
};

} // namespace qcamera
#endif
