/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3DualGeometry.h"

#include <cmath>
#include <cstdio>
#include <cstring>
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

int main() {
    testBindBinnedAux();
    testRejectBadAux();
    testFocusRoiOrigin();
    testResizeNv12();
    testFullResAux();
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("Dual geometry host tests: PASS\n");
    return 0;
}
