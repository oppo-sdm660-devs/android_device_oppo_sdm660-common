/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_EIS_SNAPSHOT_H
#define QCAMERA3_EIS_SNAPSHOT_H

// Reuses the existing MMS/EIS sources by calling their data model; neither
// QCamera3MmsVstab nor QCamera3MmsGyroSource is modified. Only the real
// QCamera3MmsVstab::Result records they produce are consumed here.
#include "QCamera3MmsVstab.h"

#include <atomic>
#include <stddef.h>
#include <stdint.h>

namespace qcamera {

// EIS-aware still selection: freeze and reuse the gyro-stabilised ZSL frame.
//
// Stock behavior behind this stage: the R11/R11s HAL exposed
// `start_eis_snapshot` / `stop_eis_snapshot`, refused with "can not run
// start_eis_snapshot,because is not in zsl mode", tracked
// `mStartEisFrameId` / `mStopEisFrameId` ("get the eis frame. mStartEisFrameId
// =%d mEisCam=%d", "mStopEisFrameId=%d for sure"), used
// `persist.camera.eis.snapshot` and scaled the EIS buffer down for the still.
// Evidence: stock-analysis item `hal.eis.snapshot`. That code is HAL1 state
// inside the plugin architecture and is not reused.
//
// This stage owns one decision: given the real EIS state and a real ZSL
// candidate list, is there a frame whose stabilisation state is usable for the
// still? If not, it refuses. It never picks an arbitrary frame and calls it
// stabilised: the documented fallback (`Options::requireStabilized == 0`)
// reports `usedEis == 0` and `applied == 0`, so no stabilisation is claimed.
//
// Explicit non-claims: it does not run the MMS core, does not read gyro data
// itself, does not verify the crop factor against a real frame, does not
// produce pixels and does not promise a sharper photo. See docs/EIS_SNAPSHOT.md.
class QCamera3EisSnapshot {
public:
    enum {
        MAX_CANDIDATES = 16,   // bounded ZSL candidate window
        MAX_FROZEN_FRAMES = 1, // one still source at a time
    };

    // Real negotiated stabilisation level. 0 = none, 1 = DIS, 2 = EIS 2.0,
    // 3 = EIS 3.0 (the stock fallback ladder).
    enum EisVersion {
        kEisNone = 0,
        kEisDis = 1,
        kEis2 = 2,
        kEis3 = 3,
    };

    enum Reason {
        kReasonSelectedStabilized = 0, // selected a frame the EIS core really warped
        kReasonSelectedFallback = 1,   // documented fallback, no stabilisation claim
        kReasonSelectedFrozen = 2,     // the already frozen frame was reused
        kReasonEisDisabled = 3,        // real state says stabilisation is off
        kReasonEisMissing = 4,         // no real gyro/engine evidence
        kReasonNoStabilizedFrame = 5,  // no candidate carries real stabilisation
        kReasonStale = 6,              // every stabilised candidate is too old
        kReasonMotion = 7,             // every stabilised candidate is too shaky
        kReasonMalformed = 8,
        kReasonFrozenInvalid = 9,      // freeze without a real stabilised selection
        kReasonNothingFrozen = 10,     // reuse without a freeze
    };

    // Real EIS state, filled from the existing MMS path
    // (QCamera3MmsVstab / QCamera3MmsGyroSource) and from the standard
    // ANDROID_CONTROL_VIDEO_STABILIZATION_MODE result.
    struct EisState {
        int32_t enabled = 0;       // 1 = stabilisation requested/active
        int32_t configured = 0;    // 1 = the MMS engine is configured with a real profile
        int32_t coreStatus = 0;    // last real core status (0 = ok)
        int32_t gyroValid = 0;     // 1 = a real gyro feed is running
        int64_t gyroPeriodNs = 0;  // real reported gyro period
        int32_t version = kEisNone;
    };

    // One real ZSL/candidate frame.
    struct Candidate {
        uint32_t frameNumber = 0;
        int64_t sensorTimestampNs = 0;
        int64_t exposureNs = 0;
        int32_t zsl = 0;           // 1 = this frame comes from the ZSL queue
        int32_t eisApplied = 0;    // 1 = the EIS core really warped THIS frame
        int32_t coreStatus = 0;    // real per-frame core status
        int32_t gyroSamples = 0;   // real gyro samples covering the exposure window
        int32_t motionMilli = -1;  // real gyro magnitude over the exposure, -1 absent
        int32_t cropMilli = 0;     // real crop factor * 1000 reported by the core
    };

    struct Options {
        // 1 = only a really stabilised frame may be selected (default). 0 is the
        // documented fallback and never reports stabilisation.
        int32_t requireStabilized = 1;
        // Real maximum age of an acceptable candidate relative to nowNs.
        int64_t maxAgeNs = 0;
        int64_t nowNs = 0;
        int32_t maxMotionMilli = 700;
        int32_t minGyroSamples = 1;
        int32_t maxCropMilli = 2000;  // sanity bound on the real crop factor
        uint32_t frameNumber = 0;     // the still/trigger frame
        static bool valid(const Options &o);
    };

    struct Selection {
        uint32_t index = 0;          // index into the candidate list
        uint32_t frameNumber = 0;
        int64_t sensorTimestampNs = 0;
        int64_t ageNs = 0;
        int32_t motionMilli = -1;
        int32_t cropMilli = 0;
        int32_t usedEis = 0;         // 1 only for a really warped frame
        int32_t reason = kReasonNoStabilizedFrame;
        int32_t considered = 0;      // candidates examined
        int32_t rejected = 0;        // candidates refused
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
        int32_t applied = 0;   // 1 only when a really stabilised frame is used
        uint32_t frameNumber = 0;
        // detail[0] = Reason, detail[1] = candidates considered,
        // detail[2] = candidates rejected, detail[3] = selected age in ms.
        int32_t detail[4] = {};
    };

    static const char *name();  // trace id "EisSnapshot"
    // Pure policy; the real EIS state comes from the MMS sources.
    static bool isAvailable();
    // Geometry of the frames the candidate list refers to.
    static bool supports(uint32_t width, uint32_t height);
    static bool validOptions(const Options &options);
    static bool validEisState(const EisState &state);
    static bool validCandidate(const Candidate &candidate);

    // Reuse of the existing MMS source: builds a candidate from the real
    // QCamera3MmsVstab::Result returned by processNext()/discard(), plus the
    // caller's real ZSL flag, gyro sample count, motion estimate and crop
    // factor. Returns false (candidate untouched) for an unusable record.
    static bool fromMmsResult(const QCamera3MmsVstab::Result &result, int32_t zsl,
            int32_t gyroSamples, int32_t motionMilli, int32_t cropMilli,
            Candidate &candidate);

    // Pure selection over the real candidate list. Selection is written only on
    // success; a refusal carries its reason in Result::detail[0].
    static int32_t select(const EisState &state, const Candidate *candidates,
            uint32_t count, const Options &options, Result &result, Selection &selection,
            const std::atomic<bool> *cancelled = nullptr);

    // Freezes one selected frame for the still. Refuses a selection that is not
    // really stabilised (never freezes an unstabilised frame as stabilised).
    int32_t freeze(const Selection &selection, int64_t nowNs, Result &result,
            const std::atomic<bool> *cancelled = nullptr);

    // Reports the frozen frame as the still source. Refuses with -ENOENT when
    // nothing is frozen; never falls back to another frame.
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
