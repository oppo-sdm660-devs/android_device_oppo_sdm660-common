/* SPDX-License-Identifier: BSD-3-Clause */
// Host test for the RAW16/DNG admission decision. The current camera ABI does
// not export a DPC hot-pixel record; RAW16/DNG must still be admitted from the
// real RAW16 and colour calibration records without fabricating a map.
#include "QCamera3RawCapability.h"

#include <errno.h>

#include <stdio.h>

using qcamera::QCamera3RawCapability;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

namespace {

QCamera3RawCapability::Inputs validInputs() {
    QCamera3RawCapability::Inputs inputs;
    inputs.activeWidth = 4640;
    inputs.activeHeight = 3488;
    inputs.levels.whiteLevel = 1023;
    inputs.levels.known = 1;
    for (unsigned i = 0; i < QCamera3RawCapability::kBlackLevelChannels; ++i)
        inputs.levels.blackLevelPattern[i] = 64;
    const int32_t illuminants[] = {21 /* D65 */, 17 /* A */};
    for (unsigned m = 0; m < QCamera3RawCapability::kDynamicRangeCount; ++m) {
        inputs.colour.illuminant[m] = illuminants[m];
        for (unsigned i = 0; i < QCamera3RawCapability::kMatrixEntries; ++i)
            inputs.colour.transform[m][i] = {i % 3 == i / 3 ? 1 : 0, 1};
    }
    inputs.colour.matrixCount = QCamera3RawCapability::kDynamicRangeCount;
    inputs.sensitivityMax = 6400;
    inputs.noiseModelKnown = 1;
    return inputs;
}

void testAdmitsWithoutHotPixelMap() {
    const auto decision = QCamera3RawCapability::decide(validInputs());
    CHECK(decision.status == 0);
    CHECK(decision.raw16Available == 1);
    CHECK(decision.dngAvailable == 1);
    CHECK(decision.completeRawCapability == 1);
    CHECK(decision.hotPixelMapAvailable == 0);
}

void testRejectsMissingCalibration() {
    auto inputs = validInputs();
    inputs.levels.known = 0;
    auto decision = QCamera3RawCapability::decide(inputs);
    CHECK(decision.raw16Available == 0);
    CHECK(decision.completeRawCapability == 0);

    inputs = validInputs();
    inputs.colour.matrixCount = 0;
    decision = QCamera3RawCapability::decide(inputs);
    CHECK(decision.dngAvailable == 0);
    CHECK(decision.completeRawCapability == 0);
}

void testRejectsMalformedHotPixelRecord() {
    auto inputs = validInputs();
    inputs.hotPixel.fromCalibration = 1;
    inputs.hotPixel.count = QCamera3RawCapability::kDpcMaxCoordinates + 1;
    const auto decision = QCamera3RawCapability::decide(inputs);
    CHECK(decision.status == -EINVAL);
    CHECK(decision.completeRawCapability == 0);
}

}  // namespace

int main() {
    testAdmitsWithoutHotPixelMap();
    testRejectsMissingCalibration();
    testRejectsMalformedHotPixelRecord();
    if (g_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    printf("raw capability host tests: PASS\n");
    return 0;
}
