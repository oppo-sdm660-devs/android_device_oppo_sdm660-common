/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef OCAM_STOCK_SUPERPHOTO_FIXTURE_H
#define OCAM_STOCK_SUPERPHOTO_FIXTURE_H
#include <stdint.h>
struct SuperPhotoFixture {
    int32_t failAt, initCalls, addedFrames, outputCalls, processCalls, releaseCalls;
    int32_t count, width, height, outWidth, outHeight, baseIndex;
    float initZoom, tuningZoom;
    int32_t tuningFront, tuningDisableCrop, tuningIso;
    uintptr_t inputs[5];
    uint8_t firstLuma[5], firstV[5], firstU[5];
    uintptr_t cancelAfterProcess;
};
extern "C" SuperPhotoFixture *superphoto_fixture();
extern "C" void superphoto_reset();
#endif
