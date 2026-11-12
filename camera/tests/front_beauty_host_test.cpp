/* SPDX-License-Identifier: BSD-3-Clause */
// Host behavioral test for QCamera3FrontBeauty against a fake libarcsoft_beautyshot.so.
// Verifies the evidenced call sequence, YV12 scratch layout and error paths; it
// does not execute or validate the proprietary algorithm.
#include "QCamera3FrontBeauty.h"
#include "QCamera3StockBeauty.h"

#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <vector>

namespace {

using qcamera::QCamera3FrontBeauty;
using qcamera::QCamera3StockBeauty;
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
    int32_t (*initArgument)() = nullptr;
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
        LOAD(initArgument, "fake_init_argument");
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
        return reset && failInit && failProcess && failUninit && tamper && initCalls && initArgument &&
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
    CHECK(fake.initCalls() == 1 && fake.uninitCalls() == 1);
    fake.failInit(1);
    CHECK(!QCamera3FrontBeauty::isAvailable());
    fake.failInit(0);
    fake.failUninit(-EIO);
    CHECK(!QCamera3FrontBeauty::isAvailable());
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
    const int32_t expectedLevels[] = {60, 0, 30, 0, 0, 0, 0};
    for (int i = 0; i < 7; ++i) {
        CHECK(fake.featureId(i) == expectedIds[i]);
        CHECK(fake.featureLevel(i) == expectedLevels[i]);
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

void testStrengthProfile(Fake &fake) {
    fake.reset();
    const int strengths[] = {0, 17, 33, 50, 67, 83, 100};
    const int soften[] = {0, 10, 30, 50, 60, 70, 80};
    for (int i = 0; i < 7; ++i) {
        const auto features = QCamera3FrontBeauty::levelsForStrength(strengths[i]);
        CHECK(features.levels[0] == soften[i]);
        CHECK(features.levels[2] == (i == 0 ? 0 : 30));
        CHECK(features.levels[1] == 0 && features.levels[3] == 0 &&
                features.levels[4] == 0 && features.levels[5] == 0 && features.levels[6] == 0);
    }
    Nv21Image image;
    const auto before = image.storage;
    CHECK(QCamera3FrontBeauty::process(image.image, 0) == 0);
    CHECK(image.storage == before && fake.initCalls() == 0);
}

void testNoChangeAndStreamingGeometry(Fake &fake) {
    fake.reset();
    Nv21Image unchanged;
    memset(unchanged.storage.data(), 0xAA, size_t(kWidth) * kHeight);
    const auto before = unchanged.storage;
    CHECK(QCamera3FrontBeauty::process(unchanged.image, 50) == -ENODATA);
    CHECK(unchanged.storage == before);

    const uint32_t geometry[][4] = {
        {640, 480, 672, 512}, {1280, 720, 1344, 736}, {1920, 1080, 1920, 1088},
    };
    for (const auto &size : geometry) {
        fake.reset();
        Image image = {};
        image.width = size[0];
        image.height = size[1];
        image.yStride = image.uvStride = size[2];
        image.yScanline = size[3];
        image.uvScanline = size[3] / 2;
        image.uvOffset = size_t(image.yStride) * image.yScanline;
        image.length = image.uvOffset + size_t(image.uvStride) * image.uvScanline;
        std::vector<uint8_t> pixels(image.length, 0x33);
        image.data = pixels.data();
        CHECK(QCamera3FrontBeauty::process(image, 83) == 0);
        CHECK(fake.width() == int32_t(size[0]) && fake.height() == int32_t(size[1]));
        CHECK(fake.pitch(0) == int32_t(size[2]));
        for (uint32_t row = 0; row < image.height; ++row) {
            CHECK(pixels[size_t(row) * image.yStride] == 0xAA);
            if (image.yStride > image.width)
                CHECK(pixels[size_t(row) * image.yStride + image.width] == 0x33);
        }
        CHECK(pixels[size_t(image.height) * image.yStride] == 0x33);
        CHECK(pixels[image.uvOffset] == 0x33);
    }
}

void testRearBeauty(Fake &fake) {
    fake.reset();
    CHECK(QCamera3StockBeauty::isAvailable());
    CHECK(fake.initCalls() == 1 && fake.uninitCalls() == 1 && fake.initArgument() == 0);
    fake.failInit(1);
    CHECK(!QCamera3StockBeauty::isAvailable());
    fake.failInit(0);
    fake.failUninit(-EIO);
    CHECK(!QCamera3StockBeauty::isAvailable());

    fake.reset();
    Nv21Image image;
    QCamera3StockBeauty::Options options;
    options.soften = 40;
    options.skinBright = 20;
    options.skinStyle = 30;
    options.softenSharp = 10;
    CHECK(QCamera3StockBeauty::process(image.image, options) == 0);
    const int ids[] = {201, 231, 234, 229};
    const int levels[] = {40, 20, 30, 10};
    CHECK(fake.featureCalls() == 4 && fake.format() == 0x802 && fake.sameImage());
    for (int i = 0; i < 4; ++i) {
        CHECK(fake.featureId(i) == ids[i]);
        CHECK(fake.featureLevel(i) == levels[i]);
    }
    const auto before = image.storage;
    CHECK(QCamera3StockBeauty::process(image.image, options) == -ENODATA);
    CHECK(image.storage == before);
    fake.failProcess(1);
    CHECK(QCamera3StockBeauty::process(image.image, options) == -EIO);
    CHECK(image.storage == before);
    fake.reset();
    std::atomic<bool> cancelled{true};
    CHECK(QCamera3StockBeauty::process(image.image, options, &cancelled) == -ECANCELED);
    CHECK(fake.initCalls() == 0 && image.storage == before);
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
    testStrengthProfile(fake);
    testNoChangeAndStreamingGeometry(fake);
    testRearBeauty(fake);
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
