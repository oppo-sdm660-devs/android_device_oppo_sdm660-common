/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3StockSuperPhoto.h"
#include "QCamera3SuperZoomPlan.h"
#include "stock_superphoto_fixture.h"
#include <atomic>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>
using Algo = qcamera::QCamera3StockSuperPhoto;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)
namespace {
struct Frame {
    std::vector<uint8_t> bytes = std::vector<uint8_t>(80 * 60 * 3 / 2 + 32, 0xEE);
    Algo::Image image = {bytes.data(), bytes.size(), 64, 48, 8, 80 * 60 + 8, 80, 80, 60, 30};
    Frame(uint8_t y = 44) {
        for (unsigned row = 0; row < 48; ++row)
            for (unsigned x = 0; x < 64; ++x) bytes[8 + row * 80 + x] = y;
        for (unsigned row = 0; row < 24; ++row)
            for (unsigned x = 0; x < 64; x += 2) {
                bytes[80 * 60 + 8 + row * 80 + x] = y + 1;
                bytes[80 * 60 + 8 + row * 80 + x + 1] = y + 2;
            }
    }
};
void test2x() {
    superphoto_reset();
    Frame inputs[4] = {Frame(30), Frame(31), Frame(32), Frame(33)}, output;
    Algo::Image images[4];
    Algo::Source sources[4];
    for (unsigned i = 0; i < 4; ++i) {
        images[i] = inputs[i].image;
        // Existing burst puts the framework reference last in capture order.
        sources[i] = {100 + i, 1000000000 + int64_t((i + 3) % 4) * 33333333, 10000000, 200};
    }
    const auto original = inputs[0].bytes;
    Algo::ZoomOptions options;
    Algo::ZoomResult result;
    CHECK(Algo::process2x(images, sources, 4, output.image, "s5k2p7sq", options, &result) == -EINVAL);
    CHECK(superphoto_fixture()->initCalls == 0);
    options.fullFieldOfView = true;
    CHECK(Algo::is2xAvailable());
    CHECK(Algo::process2x(images, sources, 4, output.image, "s5k2p7sq", options, &result) == 0);
    const auto &stats = *superphoto_fixture();
    CHECK(stats.initZoom == 2.0f && stats.tuningZoom == 2.0f);
    CHECK(stats.count == 4 && stats.addedFrames == 4 && stats.baseIndex == 0);
    CHECK(stats.width == 64 && stats.height == 48 && stats.outWidth == 64 && stats.outHeight == 48);
    CHECK(stats.tuningFront == 0 && stats.tuningDisableCrop == 0 && stats.tuningIso == 200);
    CHECK(stats.releaseCalls == 1 && result.algorithmApplied && result.submittedFrames == 4);
    CHECK(result.referenceIndex == 0 && result.zoomRatio == 2.0f);
    CHECK(result.sources[3].frameNumber == 103 && result.sources[0].timestampNs == 1099999999);
    CHECK(inputs[0].bytes == original);
    for (unsigned i = 0; i < 4; ++i) {
        CHECK(stats.inputs[i] != reinterpret_cast<uintptr_t>(inputs[i].image.data));
        CHECK(stats.firstLuma[i] == 30 + i && stats.firstV[i] == 31 + i && stats.firstU[i] == 32 + i);
    }
    CHECK(output.bytes[8] == 0xAB && output.bytes[80 * 60 + 8] == 0xCD);
    CHECK(output.bytes[0] == 0xEE && output.bytes[8 + 64] == 0xEE);
    for (int fail = 1; fail <= 7; ++fail) {
        superphoto_reset();
        superphoto_fixture()->failAt = fail;
        Frame failed;
        const auto previous = failed.bytes;
        Algo::ZoomResult untouched;
        untouched.zoomRatio = 9;
        CHECK(Algo::process2x(images, sources, 4, failed.image, "s5k2p7sq", options, &untouched) < 0);
        CHECK(failed.bytes == previous && untouched.zoomRatio == 9 && !untouched.algorithmApplied);
        CHECK(superphoto_fixture()->releaseCalls == 1);
    }
    superphoto_reset();
    sources[1].frameNumber = sources[0].frameNumber;
    CHECK(Algo::process2x(images, sources, 4, output.image, "s5k2p7sq", options, nullptr) == -EINVAL);
    sources[1].frameNumber = 101;
    sources[1].timestampNs = sources[0].timestampNs;
    CHECK(Algo::process2x(images, sources, 4, output.image, "s5k2p7sq", options, nullptr) == -EINVAL);
    sources[1].timestampNs = sources[0].timestampNs + 33333333;
    sources[2].exposureTimeNs *= 2;
    CHECK(Algo::process2x(images, sources, 4, output.image, "s5k2p7sq", options, nullptr) == -EINVAL);
    sources[2].exposureTimeNs /= 2;
    sources[2].sensitivity = 400;
    CHECK(Algo::process2x(images, sources, 4, output.image, "s5k2p7sq", options, nullptr) == -EINVAL);
    sources[2].sensitivity = 200;
    CHECK(Algo::process2x(images, sources, 3, output.image, "s5k2p7sq", options, nullptr) == -EINVAL);
    CHECK(superphoto_fixture()->initCalls == 0);
    std::atomic<bool> canceled(true);
    CHECK(Algo::process2x(images, sources, 4, output.image, "s5k2p7sq", options, nullptr, &canceled) == -ECANCELED);
    CHECK(superphoto_fixture()->initCalls == 0);
}
void testExplicitSuperIsStill1x() {
    superphoto_reset();
    Frame frame, output;
    Algo::Image images[2] = {frame.image, frame.image};
    CHECK(Algo::process(images, 2, output.image, "s5k2p7sq", 100, true) == 0);
    CHECK(superphoto_fixture()->initZoom == 1 && superphoto_fixture()->tuningZoom == 1);
    CHECK(superphoto_fixture()->tuningFront == 1 && superphoto_fixture()->count == 2);
}
void testCapturePlan() {
    using Plan = qcamera::QCamera3SuperZoomPlan;
    Plan::Conditions conditions;
    conditions.engineAvailable = conditions.rearSingleSensor =
            conditions.automaticExposure = conditions.flashOff = true;
    const Plan::Rect active = {12, 8, 4608, 3456}, crop = {1164, 872, 2304, 1728};
    Plan plan;
    CHECK(!Plan::make(false, conditions, active, crop, 4608, 3456, plan));
    CHECK(Plan::make(true, conditions, active, crop, 4608, 3456, plan));
    CHECK(plan.sourceHasFullFieldOfView(active) && !plan.sourceHasFullFieldOfView(crop));
    CHECK(plan.resultCrop.left == crop.left && plan.resultCrop.top == crop.top);
    CHECK(plan.outputWidth == 4608 && plan.outputHeight == 3456 && Plan::FRAME_COUNT == 4);
    CHECK(!Plan::make(true, conditions, active, active, 4608, 3456, plan));
    CHECK(!Plan::make(true, conditions, active, {12, 8, 2304, 1728}, 4608, 3456, plan));
    CHECK(!Plan::make(true, conditions, active, crop, 4607, 3456, plan));
    CHECK(!Plan::make(true, conditions, active, crop, 4608, 2592, plan));
    const Plan::Rect r11sActive = {0, 0, 4640, 3488};
    const Plan::Rect r11sCrop = {1160, 874, 2320, 1740};
    CHECK(Plan::make(true, conditions, r11sActive, r11sCrop, 4608, 3456, plan));
    CHECK(plan.sourceHasFullFieldOfView({0, 4, 4640, 3480}));
    CHECK(!plan.sourceHasFullFieldOfView(r11sActive));
    CHECK(!plan.sourceHasFullFieldOfView(r11sCrop));
    CHECK(!Plan::make(true, conditions, r11sActive, {1160, 872, 2320, 1744}, 4608, 3456, plan));
    CHECK(Plan::make(true, conditions, r11sActive, {1160, 1091, 2320, 1305}, 4608, 2592, plan));
    CHECK(plan.sourceHasFullFieldOfView({0, 439, 4640, 2610}));
    CHECK(Plan::make(true, conditions, r11sActive, {1448, 872, 1744, 1744}, 3456, 3456, plan));
    CHECK(plan.sourceHasFullFieldOfView({576, 0, 3488, 3488}));
    CHECK(Plan::make(true, conditions, {12, 8, 4640, 3488}, {1172, 882, 2320, 1740},
            4608, 3456, plan));
    CHECK(plan.sourceHasFullFieldOfView({12, 12, 4640, 3480}));
    bool *exclusions[] = {&conditions.video, &conditions.raw, &conditions.reprocess,
            &conditions.otherNativeStage};
    for (auto flag : exclusions) {
        *flag = true;
        CHECK(!Plan::make(true, conditions, active, crop, 4608, 3456, plan));
        *flag = false;
    }
    bool *requirements[] = {&conditions.engineAvailable, &conditions.rearSingleSensor,
            &conditions.automaticExposure, &conditions.flashOff};
    for (auto flag : requirements) {
        *flag = false;
        CHECK(!Plan::make(true, conditions, active, crop, 4608, 3456, plan));
        *flag = true;
    }
}
}
int main() {
    test2x();
    testExplicitSuperIsStill1x();
    testCapturePlan();
    puts("stock SuperPhoto 2x adapter tests passed (ABI fixture only)");
}
