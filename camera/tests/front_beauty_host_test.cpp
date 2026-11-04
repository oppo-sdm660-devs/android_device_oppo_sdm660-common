/* SPDX-License-Identifier: BSD-3-Clause */
// Host behavioral test for QCamera3FrontBeauty against a fake libarcsoft_beautyshot.so.
// Verifies the evidenced call sequence, YV12 scratch layout and error paths; it
// does not execute or validate the proprietary algorithm.
#include "QCamera3FrontBeauty.h"

#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <vector>

namespace {

using qcamera::QCamera3FrontBeauty;
using Image = QCamera3FrontBeauty::Image;

int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

struct Fake {
    void *handle = nullptr;
    void (*reset)() = nullptr;
    void (*failInit)(int32_t) = nullptr;
    void (*failProcess)(int32_t) = nullptr;
    void (*failUninit)(int32_t) = nullptr;
    void (*tamper)(int32_t) = nullptr;
    int32_t (*initCalls)() = nullptr;
    int32_t (*uninitCalls)() = nullptr;
    int32_t (*processCalls)() = nullptr;
    int32_t (*featureCalls)() = nullptr;
    int32_t (*featureId)(int32_t) = nullptr;
    int32_t (*featureLevel)(int32_t) = nullptr;
    int32_t (*sameImage)() = nullptr;
    int32_t (*faceArgNull)() = nullptr;
    int32_t (*arg5Null)() = nullptr;
    uint32_t (*format)() = nullptr;
    int32_t (*width)() = nullptr;
    int32_t (*height)() = nullptr;
    int32_t (*pitch)(int32_t) = nullptr;
    int32_t (*planesNonNull)() = nullptr;
    int32_t (*chromaStride)() = nullptr;
    int32_t (*chromaRows)() = nullptr;
    uint8_t (*chromaV)(int32_t) = nullptr;
    uint8_t (*chromaU)(int32_t) = nullptr;

    bool open() {
        handle = dlopen("libarcsoft_beautyshot.so", RTLD_NOW | RTLD_LOCAL);
        if (!handle) return false;
#define LOAD(member, symbol) member = reinterpret_cast<decltype(member)>(dlsym(handle, symbol))
        LOAD(reset, "fake_reset");
        LOAD(failInit, "fake_fail_init");
        LOAD(failProcess, "fake_fail_process");
        LOAD(failUninit, "fake_fail_uninit");
        LOAD(tamper, "fake_tamper_descriptor");
        LOAD(initCalls, "fake_init_calls");
        LOAD(uninitCalls, "fake_uninit_calls");
        LOAD(processCalls, "fake_process_calls");
        LOAD(featureCalls, "fake_feature_calls");
        LOAD(featureId, "fake_feature_id");
        LOAD(featureLevel, "fake_feature_level");
        LOAD(sameImage, "fake_same_image");
        LOAD(faceArgNull, "fake_face_arg_null");
        LOAD(arg5Null, "fake_arg5_null");
        LOAD(format, "fake_format");
        LOAD(width, "fake_width");
        LOAD(height, "fake_height");
        LOAD(pitch, "fake_pitch");
        LOAD(planesNonNull, "fake_planes_non_null");
        LOAD(chromaStride, "fake_chroma_stride");
        LOAD(chromaRows, "fake_chroma_rows");
        LOAD(chromaV, "fake_chroma_v");
        LOAD(chromaU, "fake_chroma_u");
#undef LOAD
        return reset && failInit && failProcess && failUninit && tamper && initCalls &&
                uninitCalls && processCalls && featureCalls && featureId && featureLevel &&
                sameImage && faceArgNull && arg5Null && format && width && height && pitch &&
                planesNonNull && chromaStride && chromaRows && chromaV && chromaU;
    }
};

const uint32_t kWidth = 64;
const uint32_t kHeight = 48;

struct Nv21Image {
    std::vector<uint8_t> storage;
    Image image = {};

    Nv21Image() {
        const size_t ySize = size_t(kWidth) * kHeight;
        storage.assign(ySize * 3 / 2, 0);
        for (uint32_t row = 0; row < kHeight; ++row)
            for (uint32_t col = 0; col < kWidth; ++col)
                storage[row * kWidth + col] = uint8_t((row * 7 + col * 3) & 0xff);
        for (uint32_t row = 0; row < kHeight / 2; ++row) {
            for (uint32_t col = 0; col < kWidth / 2; ++col) {
                storage[ySize + row * kWidth + 2 * col] = uint8_t(0x40 + row);
                storage[ySize + row * kWidth + 2 * col + 1] = uint8_t(0x80 + row);
            }
        }
        image.data = storage.data();
        image.length = storage.size();
        image.width = kWidth;
        image.height = kHeight;
        image.yOffset = 0;
        image.uvOffset = ySize;
        image.yStride = kWidth;
        image.uvStride = kWidth;
        image.yScanline = kHeight;
        image.uvScanline = kHeight / 2;
    }
    const uint8_t *uv() const { return storage.data() + image.uvOffset; }
};

void testAvailability(Fake &fake) {
    fake.reset();
    CHECK(QCamera3FrontBeauty::isAvailable());
}

void testSuccess(Fake &fake) {
    fake.reset();
    Nv21Image image;
    const int32_t rc = QCamera3FrontBeauty::process(image.image, 60);
    CHECK(rc == 0);
    CHECK(fake.initCalls() == 1);
    CHECK(fake.uninitCalls() == 1);
    CHECK(fake.processCalls() == 1);
    CHECK(fake.featureCalls() == 7);
    const int32_t expectedIds[] = {0xc9, 0xec, 0xca, 0xcb, 0xcd, 0xcc, 0xf0};
    for (int i = 0; i < 7; ++i) {
        CHECK(fake.featureId(i) == expectedIds[i]);
        CHECK(fake.featureLevel(i) == 60);
    }
    CHECK(fake.sameImage() == 1);
    CHECK(fake.faceArgNull() == 1);
    CHECK(fake.arg5Null() == 1);
    CHECK(fake.format() == 0x605);
    CHECK(fake.width() == int32_t(kWidth));
    CHECK(fake.height() == int32_t(kHeight));
    CHECK(fake.pitch(0) == int32_t(kWidth));
    CHECK(fake.pitch(1) == int32_t(kWidth / 2));
    CHECK(fake.pitch(2) == int32_t(kWidth / 2));
    CHECK(fake.planesNonNull() == 3);
    // The adapter must hand the core Y/V/U planes split from the NV21 (V U ...).
    CHECK(fake.chromaStride() == int32_t(kWidth / 2));
    CHECK(fake.chromaRows() == int32_t(kHeight / 2));
    for (int32_t row = 0; row < int32_t(kHeight / 2); ++row) {
        CHECK(fake.chromaV(row * int32_t(kWidth / 2)) == uint8_t(0x40 + row));
        CHECK(fake.chromaU(row * int32_t(kWidth / 2)) == uint8_t(0x80 + row));
    }
    // Fake core painted luma; chroma must round-trip unchanged into NV21.
    for (uint32_t row = 0; row < kHeight; ++row) {
        for (uint32_t col = 0; col < kWidth; ++col)
            CHECK(image.storage[row * kWidth + col] == 0xAA);
    }
    for (uint32_t row = 0; row < kHeight / 2; ++row) {
        for (uint32_t col = 0; col < kWidth / 2; ++col) {
            CHECK(image.uv()[row * kWidth + 2 * col] == uint8_t(0x40 + row));
            CHECK(image.uv()[row * kWidth + 2 * col + 1] == uint8_t(0x80 + row));
        }
    }
}

void testCancelBeforeStart(Fake &fake) {
    fake.reset();
    Nv21Image image;
    const std::atomic<bool> cancelled(true);
    const int32_t rc = QCamera3FrontBeauty::process(image.image, 30, &cancelled);
    CHECK(rc == -ECANCELED);
    CHECK(fake.initCalls() == 0);
    CHECK(fake.processCalls() == 0);
}

void testInitFailure(Fake &fake) {
    fake.reset();
    fake.failInit(1);
    Nv21Image image;
    const std::vector<uint8_t> before = image.storage;
    const int32_t rc = QCamera3FrontBeauty::process(image.image, 30);
    CHECK(rc == -EIO);
    CHECK(fake.initCalls() == 1);
    CHECK(fake.processCalls() == 0);
    CHECK(image.storage == before);
}

void testProcessFailure(Fake &fake) {
    fake.reset();
    fake.failProcess(1);
    Nv21Image image;
    const std::vector<uint8_t> before = image.storage;
    const int32_t rc = QCamera3FrontBeauty::process(image.image, 30);
    CHECK(rc == -EIO);
    CHECK(fake.processCalls() == 1);
    CHECK(fake.uninitCalls() == 1);
    CHECK(image.storage == before);
}

void testUninitFailure(Fake &fake) {
    fake.reset();
    fake.failUninit(-EIO);
    Nv21Image image;
    const std::vector<uint8_t> before = image.storage;
    const int32_t rc = QCamera3FrontBeauty::process(image.image, 30);
    CHECK(rc == -EIO);
    CHECK(fake.processCalls() == 1);
    CHECK(image.storage == before);
}

void testDescriptorTamper(Fake &fake) {
    fake.reset();
    fake.tamper(1);
    Nv21Image image;
    const std::vector<uint8_t> before = image.storage;
    const int32_t rc = QCamera3FrontBeauty::process(image.image, 30);
    CHECK(rc == -EPROTO);
    CHECK(image.storage == before);
}

void testValidation(Fake &fake) {
    fake.reset();
    Nv21Image image;
    CHECK(QCamera3FrontBeauty::process(image.image, -1) == -EINVAL);
    CHECK(QCamera3FrontBeauty::process(image.image, 101) == -EINVAL);
    CHECK(fake.initCalls() == 0);
    Image invalid = image.image;
    invalid.data = nullptr;
    CHECK(QCamera3FrontBeauty::process(invalid, 10) == -EINVAL);
    invalid = image.image;
    // Odd luma stride is rejected by the planar YV12 requirement.
    invalid.yStride = kWidth + 1;
    CHECK(QCamera3FrontBeauty::process(invalid, 10) == -EINVAL);
    CHECK(fake.initCalls() == 0);
}

} // namespace

int main() {
    Fake fake;
    if (!fake.open()) {
        fprintf(stderr, "FAIL: cannot load fake libarcsoft_beautyshot.so (LD_LIBRARY_PATH)\n");
        return 2;
    }
    testAvailability(fake);
    testSuccess(fake);
    testCancelBeforeStart(fake);
    testInitFailure(fake);
    testProcessFailure(fake);
    testUninitFailure(fake);
    testDescriptorTamper(fake);
    testValidation(fake);
    if (g_failures) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("front beauty host tests: all checks passed\n");
    return 0;
}
