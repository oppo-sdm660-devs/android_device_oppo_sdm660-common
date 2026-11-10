/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3RequestLifecycle.h"
#include "QCamera3FrameNumberMap.h"
#include <cassert>
#include <cstdio>

using qcamera::QCamera3RequestLifecycle;

int main()
{
    QCamera3RequestLifecycle queued;
    queued.returned.resize(2, false);
    assert(queued.claimFrontendCancellation());
    assert(!queued.claimFrontendCancellation());
    assert(!queued.claimShutter());

    QCamera3RequestLifecycle active;
    active.returned.resize(2, false);
    active.submitting = true;
    assert(!active.claimFrontendCancellation());
    active.submitting = false;
    assert(active.claimFrontendCancellation());

    QCamera3RequestLifecycle handedOff;
    handedOff.returned.resize(2, false);
    handedOff.submitting = true;
    handedOff.nativePending = true;
    assert(!handedOff.claimFrontendCancellation());
    handedOff.submitting = false;
    assert(handedOff.claimShutter());
    assert(!handedOff.claimShutter());
    assert(!handedOff.claimFrontendCancellation());
    handedOff.returned[0] = handedOff.returned[1] = true;
    assert(!handedOff.complete());
    handedOff.metadataDone = true;
    assert(handedOff.complete());

    QCamera3RequestLifecycle reprocess;
    reprocess.returned.resize(1, false);
    reprocess.inputReturned = false;
    reprocess.metadataDone = true;
    assert(!reprocess.complete());
    reprocess.returned[0] = true;
    assert(!reprocess.complete());
    reprocess.inputReturned = true;
    reprocess.submitting = true;
    assert(reprocess.settled());
    assert(!reprocess.complete());
    reprocess.submitting = false;
    assert(reprocess.complete());
    qcamera::QCamera3FrameNumberMap mappings;
    constexpr uint32_t slowJpeg = 800, source = 801;
    mappings.insert(slowJpeg, 1);
    mappings.insert(source, qcamera::QCamera3FrameNumberMap::InternalOnly);
    for (uint32_t frame = 802; frame < 1802; ++frame) {
        mappings.insert(frame, frame);
        mappings.retireFramework(frame);
        mappings.purge(frame + 1, 256);
    }
    uint32_t framework = 0;
    assert(mappings.get(slowJpeg, framework) && framework == 1);
    assert(mappings.get(source, framework) && framework == qcamera::QCamera3FrameNumberMap::InternalOnly);
    assert(mappings.size() <= 258);
    // Source metadata can finish while the framework JPEG remains in flight.
    mappings.retireInternal(source);
    mappings.purge(1802, 256);
    assert(!mappings.get(source, framework));
    assert(mappings.get(slowJpeg, framework) && framework == 1);
    mappings.retireFramework(1);
    mappings.purge(1802, 256);
    assert(!mappings.get(slowJpeg, framework));
    std::puts("request lifecycle tests: all checks passed");
}
