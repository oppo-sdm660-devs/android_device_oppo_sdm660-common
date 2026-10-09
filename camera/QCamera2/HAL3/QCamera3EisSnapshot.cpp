/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3EisSnapshot.h"

#include <errno.h>

namespace qcamera {
namespace {

using Eis = QCamera3EisSnapshot;

bool stopped(const std::atomic<bool> *flag) { return flag && flag->load(); }

int32_t fail(Eis::Result &result, uint32_t frameNumber, int32_t status, int32_t reason,
        int32_t detail1 = -1, int32_t detail2 = 0, int32_t detail3 = 0) {
    Eis::Result local;
    local.frameNumber = frameNumber;
    local.status = status;
    local.detail[0] = reason;
    local.detail[1] = detail1;
    local.detail[2] = detail2;
    local.detail[3] = detail3;
    result = local;
    return status;
}

int64_t ageOf(const Eis::Candidate &candidate, int64_t nowNs) {
    if (nowNs <= candidate.sensorTimestampNs) return 0;
    return nowNs - candidate.sensorTimestampNs;
}

// Deterministic ranking: newest first, then lowest real motion (an absent
// estimate ranks as 0), then the lower candidate index.
bool better(const Eis::Candidate &candidate, int64_t age, uint32_t index,
        const Eis::Candidate &best, int64_t bestAge, uint32_t bestIndex) {
    if (age != bestAge) return age < bestAge;
    const int32_t motion = candidate.motionMilli > 0 ? candidate.motionMilli : 0;
    const int32_t bestMotion = best.motionMilli > 0 ? best.motionMilli : 0;
    if (motion != bestMotion) return motion < bestMotion;
    return index < bestIndex;
}

} // namespace

bool QCamera3EisSnapshot::isAvailable() { return true; }

const char *QCamera3EisSnapshot::name() { return "EisSnapshot"; }

bool QCamera3EisSnapshot::supports(uint32_t width, uint32_t height) {
    return width >= 16 && height >= 16 && width <= 16384 && height <= 16384 &&
            !(width & 1) && !(height & 1);
}

bool QCamera3EisSnapshot::Options::valid(const Options &o) {
    return (o.requireStabilized == 0 || o.requireStabilized == 1) && o.maxAgeNs > 0 &&
            o.nowNs >= 0 && o.maxMotionMilli >= 0 && o.maxMotionMilli <= 1000 &&
            o.minGyroSamples >= 0 && o.minGyroSamples <= 100000 &&
            o.maxCropMilli >= 1000 && o.maxCropMilli <= 100000;
}

bool QCamera3EisSnapshot::validOptions(const Options &options) {
    return Options::valid(options);
}

bool QCamera3EisSnapshot::validEisState(const EisState &state) {
    return (state.enabled == 0 || state.enabled == 1) &&
            (state.configured == 0 || state.configured == 1) &&
            (state.gyroValid == 0 || state.gyroValid == 1) && state.gyroPeriodNs >= 0 &&
            state.version >= kEisNone && state.version <= kEis3;
}

bool QCamera3EisSnapshot::validCandidate(const Candidate &candidate) {
    return candidate.frameNumber > 0 && candidate.sensorTimestampNs >= 0 &&
            candidate.exposureNs >= 0 && (candidate.zsl == 0 || candidate.zsl == 1) &&
            (candidate.eisApplied == 0 || candidate.eisApplied == 1) &&
            candidate.gyroSamples >= 0 && candidate.motionMilli >= -1 &&
            candidate.motionMilli <= 1000 && candidate.cropMilli >= 0 &&
            candidate.cropMilli <= 100000;
}

bool QCamera3EisSnapshot::fromMmsResult(const QCamera3MmsVstab::Result &result,
        int32_t zsl, int32_t gyroSamples, int32_t motionMilli, int32_t cropMilli,
        Candidate &candidate) {
    if (result.frame.frameNumber == 0 || result.frame.timestampNs < 0 ||
            result.frame.exposureNs < 0) return false;
    if (zsl != 0 && zsl != 1) return false;
    if (gyroSamples < 0 || motionMilli < -1 || motionMilli > 1000) return false;
    if (cropMilli < 0 || cropMilli > 100000) return false;
    Candidate local;
    local.frameNumber = result.frame.frameNumber;
    local.sensorTimestampNs = result.frame.timestampNs;
    local.exposureNs = result.frame.exposureNs;
    local.zsl = zsl;
    local.coreStatus = result.coreStatus;
    // Only a real core success means this frame was really warped. The stock
    // crop factor is the caller's real value; `Frame::zoom` is not a crop
    // factor and is deliberately not used here.
    local.eisApplied = result.coreStatus == 0 ? 1 : 0;
    local.gyroSamples = gyroSamples;
    local.motionMilli = motionMilli;
    local.cropMilli = cropMilli;
    candidate = local;
    return true;
}

int32_t QCamera3EisSnapshot::select(const EisState &state, const Candidate *candidates,
        uint32_t count, const Options &options, Result &result, Selection &selection,
        const std::atomic<bool> *cancelFlag) {
    if (!Options::valid(options) || !validEisState(state) || count > MAX_CANDIDATES ||
            (count > 0 && !candidates)) {
        return fail(result, options.frameNumber, -EINVAL, kReasonMalformed);
    }
    if (stopped(cancelFlag)) {
        return fail(result, options.frameNumber, -ECANCELED, kReasonMalformed);
    }
    if (count == 0) {
        return fail(result, options.frameNumber, -ENOENT, kReasonNoStabilizedFrame);
    }

    const bool engineUsable = state.enabled == 1 && state.configured == 1 &&
            state.gyroValid == 1 && state.coreStatus == 0 && state.version != kEisNone &&
            state.gyroPeriodNs > 0;
    if (!engineUsable && options.requireStabilized != 0) {
        // The caller asked for a stabilised frame and the real EIS state cannot
        // provide one: refuse instead of choosing an arbitrary frame.
        const bool off = state.enabled == 0 || state.version == kEisNone;
        return fail(result, options.frameNumber, off ? -ENODEV : -ENODATA,
                off ? kReasonEisDisabled : kReasonEisMissing,
                int32_t(state.enabled), int32_t(state.version));
    }

    int32_t considered = 0, rejected = 0;
    bool stabilisedSeen = false, staleSeen = false, motionSeen = false;
    int32_t bestStabilized = -1;
    int64_t bestStabilizedAge = 0;
    int32_t bestFallback = -1;
    int64_t bestFallbackAge = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (stopped(cancelFlag)) {
            return fail(result, options.frameNumber, -ECANCELED, kReasonMalformed,
                    considered, rejected);
        }
        ++considered;
        const Candidate &candidate = candidates[i];
        if (!validCandidate(candidate) || candidate.zsl != 1) {
            ++rejected;
            continue;
        }
        const bool warped = candidate.eisApplied == 1 && candidate.coreStatus == 0;
        if (warped) stabilisedSeen = true;
        const int64_t age = ageOf(candidate, options.nowNs);
        if (age > options.maxAgeNs) {
            staleSeen = true;
            ++rejected;
            continue;
        }
        if (warped) {
            if (candidate.gyroSamples < options.minGyroSamples) {
                ++rejected;
                continue;
            }
            if (candidate.motionMilli > options.maxMotionMilli) {
                motionSeen = true;
                ++rejected;
                continue;
            }
            if (candidate.cropMilli != 0 &&
                    (candidate.cropMilli < 1000 || candidate.cropMilli > options.maxCropMilli)) {
                ++rejected;
                continue;
            }
            if (bestStabilized < 0 || better(candidate, age, i,
                    candidates[uint32_t(bestStabilized)], bestStabilizedAge,
                    uint32_t(bestStabilized))) {
                bestStabilized = int32_t(i);
                bestStabilizedAge = age;
            }
        } else if (options.requireStabilized == 0) {
            // Documented fallback candidate: a real ZSL frame that the EIS core
            // did not warp. It is never reported as stabilised.
            if (candidate.motionMilli > options.maxMotionMilli) {
                motionSeen = true;
                ++rejected;
                continue;
            }
            if (bestFallback < 0 || better(candidate, age, i,
                    candidates[uint32_t(bestFallback)], bestFallbackAge,
                    uint32_t(bestFallback))) {
                bestFallback = int32_t(i);
                bestFallbackAge = age;
            }
        } else {
            ++rejected;
        }
    }

    int32_t chosen = bestStabilized;
    int32_t usedEis = 1;
    int32_t reason = kReasonSelectedStabilized;
    if (chosen < 0 && options.requireStabilized == 0) {
        chosen = bestFallback;
        usedEis = 0;
        reason = kReasonSelectedFallback;
    }
    if (chosen < 0) {
        if (stabilisedSeen && staleSeen) {
            return fail(result, options.frameNumber, -ENOENT, kReasonStale,
                    considered, rejected);
        }
        if (stabilisedSeen && motionSeen) {
            return fail(result, options.frameNumber, -ENOENT, kReasonMotion,
                    considered, rejected);
        }
        return fail(result, options.frameNumber, -ENOENT, kReasonNoStabilizedFrame,
                considered, rejected);
    }

    const Candidate &best = candidates[uint32_t(chosen)];
    const int64_t age = ageOf(best, options.nowNs);
    Selection local;
    local.index = uint32_t(chosen);
    local.frameNumber = best.frameNumber;
    local.sensorTimestampNs = best.sensorTimestampNs;
    local.ageNs = age;
    local.motionMilli = best.motionMilli;
    local.cropMilli = best.cropMilli;
    local.usedEis = usedEis;
    local.reason = reason;
    local.considered = considered;
    local.rejected = rejected;
    selection = local;

    Result selected;
    selected.status = 0;
    selected.applied = usedEis;
    selected.frameNumber = options.frameNumber;
    selected.detail[0] = reason;
    selected.detail[1] = considered;
    selected.detail[2] = rejected;
    selected.detail[3] = int32_t(age / 1000000);
    result = selected;
    return 0;
}

int32_t QCamera3EisSnapshot::freeze(const Selection &selection, int64_t nowNs,
        Result &result, const std::atomic<bool> *cancelFlag) {
    if (nowNs < 0) {
        return fail(result, selection.frameNumber, -EINVAL, kReasonMalformed);
    }
    if (stopped(cancelFlag)) {
        return fail(result, selection.frameNumber, -ECANCELED, kReasonMalformed);
    }
    if (selection.usedEis != 1 || selection.frameNumber == 0) {
        // An unstabilised frame is never frozen as the stabilised still source.
        return fail(result, selection.frameNumber, -ENODATA, kReasonFrozenInvalid,
                int32_t(selection.usedEis));
    }
    if (mFrozen.active && mFrozen.frameNumber != selection.frameNumber) {
        // One still source at a time: the caller must release the held frame.
        return fail(result, selection.frameNumber, -EBUSY, kReasonFrozenInvalid,
                int32_t(mFrozen.frameNumber));
    }

    Frozen local;
    local.active = 1;
    local.frameNumber = selection.frameNumber;
    local.index = selection.index;
    local.sensorTimestampNs = selection.sensorTimestampNs;
    local.frozenAtNs = nowNs;
    local.motionMilli = selection.motionMilli;
    local.cropMilli = selection.cropMilli;
    local.usedEis = 1;
    mFrozen = local;

    Result frozen;
    frozen.status = 0;
    frozen.applied = 1;
    frozen.frameNumber = selection.frameNumber;
    frozen.detail[0] = kReasonSelectedStabilized;
    frozen.detail[1] = int32_t(selection.index);
    frozen.detail[3] = int32_t(selection.ageNs / 1000000);
    result = frozen;
    return 0;
}

int32_t QCamera3EisSnapshot::reuse(uint32_t frameNumber, Result &result,
        Selection &selection, const std::atomic<bool> *cancelFlag) const {
    if (stopped(cancelFlag)) {
        return fail(result, frameNumber, -ECANCELED, kReasonMalformed);
    }
    if (mFrozen.active != 1) {
        return fail(result, frameNumber, -ENOENT, kReasonNothingFrozen);
    }

    Selection local;
    local.index = mFrozen.index;
    local.frameNumber = mFrozen.frameNumber;
    local.sensorTimestampNs = mFrozen.sensorTimestampNs;
    local.ageNs = 0;
    local.motionMilli = mFrozen.motionMilli;
    local.cropMilli = mFrozen.cropMilli;
    local.usedEis = mFrozen.usedEis;
    local.reason = kReasonSelectedFrozen;
    local.considered = 1;
    local.rejected = 0;
    selection = local;

    Result reused;
    reused.status = 0;
    reused.applied = mFrozen.usedEis;
    reused.frameNumber = frameNumber;
    reused.detail[0] = kReasonSelectedFrozen;
    reused.detail[1] = 1;
    result = reused;
    return 0;
}

void QCamera3EisSnapshot::release() {
    mFrozen = Frozen();
}

void QCamera3EisSnapshot::reset() {
    mFrozen = Frozen();
}

} // namespace qcamera
