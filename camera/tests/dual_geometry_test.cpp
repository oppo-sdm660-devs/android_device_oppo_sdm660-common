/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3DualGeometry.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <vector>

using namespace qcamera;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static QCamera3DualGeometry::Calibration fakeCalibration() {
    QCamera3DualGeometry::Calibration calibration;
    calibration.source = QCamera3DualGeometry::Calibration::CALIBRATED_OUTPUT;
    for (size_t i = 0; i < sizeof(calibration.data); i += 2)
        calibration.data[i] = uint8_t(0x20 + (i & 0x3f));
    const uint32_t offsets[] = {0x0c, 0x28, 0xa0, 0xa4, 0xa8, 0xac, 0xb0, 0xb4,
            0xd8, 0xdc, 0xe0, 0xe4, 0xe8, 0xec};
    for (uint32_t offset : offsets) {
        const float value = 1.0f;
        memcpy(calibration.data + offset, &value, sizeof(value));
    }
    const uint16_t one = 1;
    const uint32_t shorts[] = {0x10, 0x12, 0x2c, 0x2e};
    for (uint32_t offset : shorts) memcpy(calibration.data + offset, &one, sizeof(one));
    return calibration;
}

static QCamera3DualGeometry::SensorInfo sensor(uint32_t pixelW, uint32_t pixelH,
        QCamera3DualGeometry::Rect active, float focal, float hfov, float vfov) {
    QCamera3DualGeometry::SensorInfo info = {};
    info.pixelWidth = pixelW; info.pixelHeight = pixelH; info.activeArray = active;
    info.mountAngle = 90; info.focalLength = focal;
    info.horizontalFov = hfov; info.verticalFov = vfov;
    return info;
}

static void testBindBinnedAux() {
    const QCamera3DualGeometry::Calibration calibration = fakeCalibration();
    const QCamera3DualGeometry::SensorInfo sensors[2] = {
        sensor(4640, 3488, {0, 0, 4640, 3488}, 4.27f, 62.0f, 48.5f),
        sensor(5200, 3896, {8, 8, 5184, 3880}, 5.47f, 49.9f, 38.6f)};
    QCamera3DualGeometry::Profile profile;
    CHECK(QCamera3DualGeometry::fromCapabilities(calibration, sensors, profile) == 0);
    CHECK(profile.sensors[0].capture.imageWidth == 3264);
    CHECK(profile.sensors[1].capture.imageWidth == 5120);
    const uint32_t handles[2] = {0x100, 0x200};
    const uint32_t widths[2] = {4640, 2592};   // aux binned actual output
    const uint32_t heights[2] = {3488, 1940};
    QCamera3DualCapture::Config config = {};
    CHECK(QCamera3DualGeometry::bind(profile, calibration, sensors, handles,
            widths, heights, config) == 0);
    CHECK(config.sensors[0].sourceWidth == 3264 && config.sensors[0].sourceHeight == 2448);
    CHECK(config.sensors[1].sourceWidth == 5120 && config.sensors[1].sourceHeight == 3848);
    CHECK(config.sensors[1].imageWidth == 5120 && config.sensors[1].imageHeight == 3848);
    CHECK(config.wideMaxFov > 61.9f && config.wideMaxFov < 62.1f);
    CHECK(config.teleMaxFov > 49.8f && config.teleMaxFov < 50.0f);
    CHECK(QCamera3DualCapture::validConfig(config));
}

static void testRejectBadAux() {
    const QCamera3DualGeometry::Calibration calibration = fakeCalibration();
    const QCamera3DualGeometry::SensorInfo sensors[2] = {
        sensor(4640, 3488, {0, 0, 4640, 3488}, 4.27f, 62.0f, 48.5f),
        sensor(5200, 3896, {8, 8, 5184, 3880}, 5.47f, 49.9f, 38.6f)};
    QCamera3DualGeometry::Profile profile;
    CHECK(QCamera3DualGeometry::fromCapabilities(calibration, sensors, profile) == 0);
    const uint32_t handles[2] = {1, 2};
    QCamera3DualCapture::Config config = {};
    // Aspect 16:9 does not match the 4:3 reference: must stay a hard failure.
    const uint32_t badWidths[2] = {4640, 2592};
    const uint32_t badHeights[2] = {3488, 1458};
    CHECK(QCamera3DualGeometry::bind(profile, calibration, sensors, handles,
            badWidths, badHeights, config) == -ERANGE);
    CHECK(!config.sensors[1].sourceWidth);
}

static void testFocusRoiOrigin() {
    const QCamera3DualCapture::Rect roi = {100, 200, 1000, 800};
    int32_t x = 0, y = 0;
    CHECK(QCamera3DualCapture::mapFocus(roi, 3264, 2448, 600, 600, x, y));
    CHECK(x == 1632 && y == 1224);
    CHECK(!QCamera3DualCapture::mapFocus(roi, 3264, 2448, 99, 600, x, y));
    CHECK(!QCamera3DualCapture::mapFocus({0, 0, 0, 0}, 3264, 2448, 1, 1, x, y));
}


static QCamera3DualCapture::Image makeImage(std::vector<uint8_t> &storage,
        uint32_t width, uint32_t height) {
    storage.assign(size_t(width) * height * 3 / 2, 0);
    QCamera3DualCapture::Image image = {};
    image.data = storage.data(); image.length = storage.size();
    image.width = width; image.height = height;
    image.yOffset = 0; image.uvOffset = size_t(width) * height;
    image.yStride = width; image.uvStride = width;
    image.yScanline = height; image.uvScanline = height / 2;
    for (uint32_t y = 0; y < height; ++y)
        memset(storage.data() + size_t(y) * width, 0x10, width);
    for (uint32_t y = 0; y < height / 2; ++y)
        for (uint32_t x = 0; x < width; x += 2) {
            storage[image.uvOffset + size_t(y) * width + x] = 0x80;
            storage[image.uvOffset + size_t(y) * width + x + 1] = 0x40;
        }
    return image;
}

static void testResizeNv12() {
    std::vector<uint8_t> srcStorage, dstStorage;
    auto src = makeImage(srcStorage, 2592, 1940);
    auto dst = makeImage(dstStorage, 5120, 3848);
    memset(dstStorage.data(), 0, dstStorage.size());
    CHECK(QCamera3DualCapture::resize(src, dst, false));
    CHECK(dst.data[dst.yOffset + 100 * dst.yStride + 100] == 0x10);
    CHECK(dst.data[dst.uvOffset + 50 * dst.uvStride + 100] == 0x80);
    CHECK(dst.data[dst.uvOffset + 50 * dst.uvStride + 101] == 0x40);

    std::vector<uint8_t> wideStorage, narrowStorage, portraitStorage;
    auto wide = makeImage(wideStorage, 4640, 3488);
    auto narrow = makeImage(narrowStorage, 3264, 2448);
    CHECK(QCamera3DualCapture::resize(wide, narrow, false));
    CHECK(narrow.data[narrow.yOffset] == 0x10);
    CHECK(narrow.data[narrow.uvOffset] == 0x80);

    auto portrait = makeImage(portraitStorage, 3264, 2448);
    CHECK(QCamera3DualCapture::resize(src, portrait, true, 0.775f));
    CHECK(portrait.data[portrait.yOffset] == 0x10);
}

static void testFullResAux() {
    const QCamera3DualGeometry::Calibration calibration = fakeCalibration();
    const QCamera3DualGeometry::SensorInfo sensors[2] = {
        sensor(4640, 3488, {0, 0, 4640, 3488}, 4.27f, 63.6f, 50.0f),
        sensor(5200, 3896, {8, 8, 5184, 3880}, 5.47f, 49.5f, 38.3f)};
    QCamera3DualGeometry::Profile profile;
    CHECK(QCamera3DualGeometry::fromCapabilities(calibration, sensors, profile) == 0);
    QCamera3DualCapture::Config config = {};
    const uint32_t widths[2] = {4640, 5184};
    const uint32_t heights[2] = {3488, 3880};
    const uint32_t handles[2] = {1, 2};
    CHECK(QCamera3DualGeometry::bind(profile, calibration, sensors, handles,
            widths, heights, config) == 0);
    CHECK(config.sensors[1].sourceWidth == 5184 && config.sensors[1].sourceHeight == 3880);
    CHECK(QCamera3DualCapture::validConfig(config));
    CHECK(config.wideMaxFov > 63.5f && config.teleMaxFov > 49.4f && config.teleMaxFov < 49.6f);
    QCamera3DualCapture::Config invalid = config;
    invalid.teleMaxFov = invalid.wideMaxFov + 1.0f;
    CHECK(!QCamera3DualCapture::validConfig(invalid));
    invalid = config;
    invalid.sensors[1].sourceWidth = 2592;
    invalid.sensors[1].sourceHeight = 1458; // 16:9, >1% aspect mismatch
    CHECK(!QCamera3DualCapture::validConfig(invalid));
}

static void testPortraitFramingUsesOpticalGeometry() {
    using Framing = QCamera3DualCapture::PortraitFraming;
    const QCamera3DualGeometry::SensorInfo sensors[2] = {
        sensor(4640, 3488, {8, 8, 4624, 3472}, 4.27f, 62.0f, 48.5f),
        sensor(5200, 3896, {8, 8, 5184, 3880}, 5.47f, 49.9f, 38.6f)};
    const QCamera3DualCapture::Rect source = {108, 208, 4000, 3000};
    QCamera3DualCapture::Rect half, full;
    CHECK(QCamera3DualGeometry::portraitCrop(sensors, Framing::Half, source, half) == 0);
    CHECK(QCamera3DualGeometry::portraitCrop(sensors, Framing::Full, source, full) == 0);
    CHECK(full.left == source.left && full.top == source.top &&
            full.width == source.width && full.height == source.height);
    const float ratio = tanf(49.9f * float(M_PI / 360)) / tanf(62.0f * float(M_PI / 360));
    CHECK(half.width == (int32_t(source.width * ratio) & ~1));
    CHECK(half.height == (int32_t(source.height * ratio) & ~1));
    CHECK((half.left | half.top | half.width | half.height) % 2 == 0);
    CHECK(half.left > source.left && half.top > source.top);
    CHECK(QCamera3DualGeometry::portraitCrop(sensors, Framing(2), source, full) == -ERANGE);
    CHECK(full.width == 0);
    CHECK(QCamera3DualGeometry::portraitCrop(sensors, Framing::Half,
            {0, 0, 4000, 3000}, full) == -EINVAL);
    QCamera3DualCapture::Config config = {};
    config.wideMaxFov = 62; config.teleMaxFov = 49.9f;
    QCamera3DualCapture::Rect imageCrop;
    CHECK(QCamera3DualCapture::framingCrop(config, Framing::Half, 64, 48, imageCrop));
    std::vector<uint8_t> sourceBytes, halfBytes, fullBytes;
    const auto input = makeImage(sourceBytes, 64, 48);
    const auto halfImage = makeImage(halfBytes, 64, 48);
    const auto fullImage = makeImage(fullBytes, 64, 48);
    for (uint32_t y = 0; y < input.height; ++y)
        for (uint32_t x = 0; x < input.width; ++x) sourceBytes[y * 64 + x] = uint8_t(x + 2 * y);
    CHECK(QCamera3DualCapture::resize(input, halfImage, true, ratio));
    CHECK(QCamera3DualCapture::resize(input, fullImage, false));
    CHECK(fullBytes[0] == 0 && fullBytes[47 * 64 + 63] == 157);
    CHECK(halfBytes[0] == sourceBytes[imageCrop.top * 64 + imageCrop.left]);
    CHECK(halfBytes[0] > fullBytes[0]);
    CHECK(halfImage.width == fullImage.width && halfImage.height == fullImage.height);
    CHECK(halfBytes[64 * 48] == 0x80 && halfBytes[64 * 48 + 1] == 0x40);
    config.teleMaxFov = 0;
    CHECK(!QCamera3DualCapture::framingCrop(config, Framing::Half, 64, 48, imageCrop));
    CHECK(QCamera3DualCapture::framingCrop(config, Framing::Full, 64, 48, imageCrop));
}

static void testPairedProcessFraming() {
    using Capture = QCamera3DualCapture;
    using Framing = Capture::PortraitFraming;
    void *library = dlopen("libarcsoft_dualcam_refocus.so", RTLD_NOW);
    CHECK(library != nullptr);
    if (!library) return;
    auto mode = reinterpret_cast<void (*)(int)>(dlsym(library, "fake_dual_mode"));
    auto inits = reinterpret_cast<int (*)()>(dlsym(library, "fake_dual_inits"));
    auto uninits = reinterpret_cast<int (*)()>(dlsym(library, "fake_dual_uninits"));
    auto calculates = reinterpret_cast<int (*)()>(dlsym(library, "fake_dual_calculates"));
    auto processes = reinterpret_cast<int (*)()>(dlsym(library, "fake_dual_processes"));
    CHECK(mode && inits && uninits && calculates && processes);
    if (!mode || !inits || !uninits || !calculates || !processes) {
        dlclose(library);
        return;
    }
    Capture::Config config = {};
    for (int i = 0; i < 2; ++i) {
        auto &sensor = config.sensors[i];
        sensor.cameraHandle = i + 11;
        sensor.referenceWidth = sensor.imageWidth = sensor.sourceWidth = 64;
        sensor.referenceHeight = sensor.imageHeight = sensor.sourceHeight = 48;
        sensor.referenceCrop = sensor.activeArrayCrop = {0, 0, 64, 48};
    }
    config.wideMaxFov = 62; config.teleMaxFov = 49.9f;
    config.maxTimestampDeltaNs = 10000000; config.timeoutNs = 2000000000;
    const auto calibration = fakeCalibration();
    Capture capture;
    Capture::Request request = {{7, 8}, 32, 24, 20};
    uint64_t token = 0;
    std::vector<uint8_t> wideBytes, auxBytes, destinationBytes, expectedBytes;
    const auto wide = makeImage(wideBytes, 64, 48);
    const auto aux = makeImage(auxBytes, 64, 48);
    const auto destination = makeImage(destinationBytes, 64, 48);
    const auto expected = makeImage(expectedBytes, 64, 48);
    for (uint32_t y = 0; y < 48; ++y)
        for (uint32_t x = 0; x < 64; ++x) wideBytes[y * 64 + x] = uint8_t(x + 2 * y);
    for (uint32_t y = 0; y < 24; ++y)
        for (uint32_t x = 0; x < 64; x += 2) {
            wideBytes[64 * 48 + y * 64 + x] = uint8_t(x + y);
            wideBytes[64 * 48 + y * 64 + x + 1] = uint8_t(200 - x - y);
        }
    std::fill(auxBytes.begin(), auxBytes.end(), 200);
    const auto original = wideBytes;
    const auto submit = [&](int64_t delta) {
        for (int i = 0; i < 2; ++i) {
            Capture::Metadata metadata = {};
            metadata.timestampNs = 1000000000 + i * delta;
            metadata.exposureNs = 10000000; metadata.frameDurationNs = 33000000;
            metadata.sensitivity = 100; metadata.crop = {0, 0, 64, 48};
            CHECK(capture.submitMetadata(token, i + 11, request.frameIds[i], metadata) == 0);
            CHECK(capture.submitImage(token, i + 11, request.frameIds[i], i ? aux : wide) == 0);
        }
    };
    const float ratio = tanf(49.9f * float(M_PI / 360)) / tanf(62.0f * float(M_PI / 360));
    for (const auto framing : {Framing::Half, Framing::Full}) {
        request.framing = framing;
        mode(0);
        wideBytes = original;
        CHECK(capture.begin(config, request, calibration, token) == 0 && token);
        CHECK(capture.tokenFor(11, 7) == token && !capture.tokenFor(11, 8));
        CHECK(capture.process(token, destination) == -EAGAIN);
        submit(1000000);
        CHECK(capture.submitImage(token, 11, 7, wide) == -EALREADY);
        CHECK(capture.isReady(token));
        std::fill(wideBytes.begin(), wideBytes.end(), 0);
        CHECK(capture.process(token, destination) == 0);
        wideBytes = original;
        CHECK(Capture::resize(wide, expected, framing == Framing::Half, ratio));
        CHECK(destinationBytes == expectedBytes);
        CHECK(inits() == 1 && uninits() == 1 && calculates() == 1 && processes() == 1);
        CHECK(!capture.hasActiveCapture() && !capture.tokenFor(11, 7));
    }
    for (int failure : {0, 1}) {
        mode(failure);
        CHECK(capture.begin(config, request, calibration, token) == 0);
        submit(failure == 0 ? 11000000 : 1000000);
        std::fill(destinationBytes.begin(), destinationBytes.end(), 0x5a);
        const auto before = destinationBytes;
        CHECK(capture.process(token, destination) == (failure == 0 ? -ERANGE : -EIO));
        CHECK(destinationBytes == before && !capture.hasActiveCapture());
        CHECK(inits() == failure && uninits() == failure && processes() == 0);
    }
    mode(0);
    CHECK(capture.begin(config, request, calibration, token) == 0);
    submit(1000000);
    CHECK(capture.cancel(token));
    CHECK(capture.process(token, destination) == -ECANCELED && inits() == 0);
    request.framing = Framing(2);
    CHECK(capture.begin(config, request, calibration, token) == -EINVAL && token == 0);
    dlclose(library);
}

int main() {
    testBindBinnedAux();
    testRejectBadAux();
    testFocusRoiOrigin();
    testResizeNv12();
    testFullResAux();
    testPortraitFramingUsesOpticalGeometry();
    testPairedProcessFraming();
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("Dual geometry host tests: PASS\n");
    return 0;
}
