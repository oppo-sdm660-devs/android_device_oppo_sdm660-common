/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_REQUEST_LIFECYCLE_H
#define QCAMERA3_REQUEST_LIFECYCLE_H

#include <algorithm>
#include <vector>

namespace qcamera {

// One accepted framework request has one cancellation owner across handoff.
struct QCamera3RequestLifecycle {
    std::vector<bool> returned;
    bool nativePending = false;
    bool submitting = false;
    bool metadataDone = false;
    bool inputReturned = true;
    bool frontendSettled = false;
    bool shutterSent = false;
    bool claimShutter() {
        if (shutterSent || frontendSettled) return false;
        shutterSent = true;
        return true;
    }

    bool settled() const {
        return metadataDone && inputReturned &&
                std::all_of(returned.begin(), returned.end(), [](bool value) { return value; });
    }
    bool complete() const { return !submitting && settled(); }
    bool claimFrontendCancellation() {
        if (nativePending || submitting || frontendSettled) return false;
        frontendSettled = true;
        return true;
    }
};

} // namespace qcamera
#endif
