/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3MmsVstab.h"
#include "QCamera3MmsProfile.h"
#include "QCamera3GyroVstab.h"

#include <algorithm>
#include <atomic>
#include <dlfcn.h>
#include <errno.h>
#include <limits>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>

using qcamera::QCamera3MmsVstab;
using qcamera::QCamera3MmsProfile;
using qcamera::QCamera3GyroVstab;
#define CHECK(expression) do { if (!(expression)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression); exit(1); } } while (0)
struct Buffer {
    std::vector<uint8_t> pixels;
    QCamera3MmsVstab::Image image;
    Buffer(uint32_t width, uint32_t height, uint32_t padding = 0)
        : pixels(size_t(width + padding) * (height + 2) * 3 / 2, 0xcc),
          image{pixels.data(), pixels.size(), width, height, 0,
                size_t(width + padding) * (height + 2), width + padding, width + padding,
                height + 2, (height + 2) / 2} {}
    void pattern() {
        for (uint32_t y = 0; y < image.height; ++y)
            for (uint32_t x = 0; x < image.width; ++x)
                pixels[y * image.yStride + x] = uint8_t(17 + x * 7 + y * 13);
        for (uint32_t y = 0; y < image.height / 2; ++y)
            for (uint32_t x = 0; x < image.width; ++x)
                pixels[image.uvOffset + y * image.uvStride + x] = uint8_t(61 + x * 9 + y * 19);
    }
};
void sameActive(const Buffer &left, const Buffer &right) {
    CHECK(left.image.width == right.image.width && left.image.height == right.image.height);
    for (unsigned plane = 0; plane < 2; ++plane)
        for (uint32_t y = 0; y < left.image.height / (plane ? 2 : 1); ++y)
            for (uint32_t x = 0; x < left.image.width; ++x) {
                size_t l = (plane ? left.image.uvOffset : 0) + y * left.image.yStride + x;
                size_t r = (plane ? right.image.uvOffset : 0) + y * right.image.yStride + x;
                CHECK(std::abs(int(left.pixels[l]) - int(right.pixels[r])) <= 1);
            }
}
void writeProfile(const char *path, uint32_t width = 16, uint32_t height = 12,
        int timer = 7, bool duplicate = false) {
    FILE *file = fopen(path, "w");
    CHECK(file);
    fprintf(file, "frmWidth = %u\nfrmHeight = %u\ngridVrtxCntH = 3\n"
            "gridVrtxCntV = 3\nisGridNormalized = 1\ngyroTmrInx = %d\n"
            "frmTmrInx = 7\naverageGyroTime = 5000000\n", width, height, timer);
    if (duplicate) fputs("frmTmrInx = 7\n", file);
    fclose(file);
}
int main(int argc, char **argv) {
    CHECK(argc == 3);
    void *fake = dlopen("libmms_gyro_vstab.so", RTLD_NOW | RTLD_LOCAL);
    CHECK(fake);
    auto reset = reinterpret_cast<void (*)()>(dlsym(fake, "fakeMmsReset"));
    auto set = reinterpret_cast<void (*)(int, int)>(dlsym(fake, "fakeMmsSet"));
    auto get = reinterpret_cast<int (*)(int)>(dlsym(fake, "fakeMmsGet"));
    auto lastTimestamp = reinterpret_cast<int64_t (*)()>(dlsym(fake, "fakeMmsLastTimestamp"));
    CHECK(reset && set && get && lastTimestamp);
    reset();
    CHECK(QCamera3MmsVstab::isAvailable());
    const std::string path = std::string(argv[1]) + "/test.config";
    writeProfile(path.c_str());
    QCamera3MmsVstab::Configuration config = {path.c_str(), 16, 12, 8, 6, 8, 250000000};
    CHECK(QCamera3MmsVstab::probe(config) == 0);
    CHECK(get(0) == 1 && get(1) == 1 && get(2) == 0 && get(3) == 0);
    set(0, 19);
    CHECK(QCamera3MmsVstab::probe(config) == -EIO);
    set(0, 0); set(3, 1);
    CHECK(QCamera3MmsVstab::probe(config) == -EPROTO);
    CHECK(get(1) == 2);
    set(3, 0);
    writeProfile(path.c_str(), 16, 12, 6);
    const int creates = get(0);
    CHECK(QCamera3MmsVstab::probe(config) == -EINVAL && get(0) == creates);
    writeProfile(path.c_str(), 16, 12, 7, true);
    CHECK(QCamera3MmsVstab::probe(config) == -EINVAL);
    writeProfile(path.c_str());
    auto invalid = config; invalid.outputHeight = 4;
    CHECK(QCamera3MmsVstab::probe(invalid) == -EINVAL);
    invalid = config; invalid.profilePath = "/nonexistent/mms.config";
    CHECK(QCamera3MmsVstab::probe(invalid) == -ENOENT);

    // Both evidenced sensors and resolutions use their own real installed profile.
    for (const char *sensor : {"s5k2p7sq", "imx398"}) {
        for (uint32_t width : {1280U, 1920U}) {
            QCamera3MmsProfile profile;
            CHECK(QCamera3MmsProfile::select(sensor, "BMI160 Gyroscope", "BOSCH",
                    width, width == 1280 ? 720 : 1080, profile));
            std::string actual = std::string(argv[2]) + profile.path;
            QCamera3MmsVstab::Configuration installed = {actual.c_str(), profile.inputWidth,
                profile.inputHeight, profile.outputWidth, profile.outputHeight, 8, 250000000};
            CHECK(QCamera3MmsVstab::probe(installed) == 0);
        }
    }
    QCamera3MmsProfile rejected;
    CHECK(!QCamera3MmsProfile::select("imx398", "Other Gyroscope", "BOSCH", 1280, 720, rejected));
    CHECK(!QCamera3MmsProfile::select("imx398", "BMI160 Gyroscope", "BOSCH", 3840, 2160, rejected));

    Buffer source(16, 12, 8), destination(8, 6, 4), expected(8, 6);
    source.pattern();
    CHECK(QCamera3MmsVstab::renderPassthrough(source.image, expected.image) == 0);
    QCamera3MmsVstab engine;
    CHECK(engine.configure(config) == 0 && engine.gyroPeriodNs() == 5000000);
    const float rate[] = {0.1f, -0.2f, 0.3f};
    CHECK(engine.addGyro(1000000000, rate) == 0);
    CHECK(engine.addGyro(1000000000, rate) == -EINVAL);
    const float nan[] = {std::numeric_limits<float>::quiet_NaN(), 0, 0};
    CHECK(engine.addGyro(1005000000, nan) == -EINVAL);
    set(2, 23);
    CHECK(engine.addGyro(1005000000, rate) == -EIO);
    CHECK(engine.addGyroFailures() == 1);
    set(2, 0);
    CHECK(engine.addGyro(1005000000, rate) == 0);
    QCamera3MmsVstab::Frame frame = {41, 1010000000, 10000000, 1.0f};
    CHECK(engine.enqueue(source.image, frame, frame.timestampNs) == 0);
    const auto original = source.pixels;
    std::fill(source.pixels.begin(), source.pixels.end(), 0);
    QCamera3MmsVstab::Result result;
    // Retrying preserves both the copied frame and its capture index.
    for (int retry : {2, 4, 5, 27, 20000, 20001}) {
        set(1, retry);
        CHECK(engine.processNext(destination.image, frame.timestampNs + 5000000, result) == -EAGAIN);
        CHECK(engine.pending() == 1 && result.sessionFrameIndex == 1);
        CHECK(result.frame.frameNumber == 41 && lastTimestamp() == frame.timestampNs);
        CHECK(std::all_of(destination.pixels.begin(), destination.pixels.end(),
                [](uint8_t value) { return value == 0xcc; }));
    }
    set(1, 0);
    CHECK(engine.processNext(destination.image, frame.timestampNs + 10000000, result) == 0);
    CHECK(result.fallbackReason == 0 && result.sessionFrameIndex == 1 && engine.pending() == 0);
    CHECK(result.gyroDeltaNs == 5000000 && result.addGyroFailures == 1);
    sameActive(destination, expected);
    source.pixels = original;
    source.image.data = source.pixels.data();

    ++frame.frameNumber; frame.timestampNs += 33333333;
    CHECK(engine.enqueue(source.image, frame, frame.timestampNs) == 0);
    set(1, 5);
    CHECK(engine.processNext(destination.image, frame.timestampNs + 250000000, result) == 0);
    CHECK(result.fallbackReason == -ETIMEDOUT && result.sessionFrameIndex == 2);
    sameActive(destination, expected);
    set(1, 0); set(4, 1);
    ++frame.frameNumber; frame.timestampNs += 33333333;
    CHECK(engine.enqueue(source.image, frame, frame.timestampNs) == 0);
    CHECK(engine.processNext(destination.image, frame.timestampNs, result) == 0);
    CHECK(result.fallbackReason == -EPROTO);
    sameActive(destination, expected);
    set(4, 0); set(1, 55);
    CHECK(engine.configure(config) == 0);
    for (unsigned i = 0; i < 8; ++i) {
        ++frame.frameNumber; frame.timestampNs += 33333333;
        CHECK(engine.enqueue(source.image, frame, frame.timestampNs) == 0);
        CHECK(engine.processNext(destination.image, frame.timestampNs, result) == 0);
        CHECK(result.fallbackReason == -EIO && result.coreStatus == 55);
    }
    CHECK(engine.coreDegraded());
    const int warps = get(2);
    ++frame.frameNumber; frame.timestampNs += 33333333;
    CHECK(engine.enqueue(source.image, frame, frame.timestampNs) == 0);
    CHECK(engine.processNext(destination.image, frame.timestampNs, result) == 0);
    CHECK(result.fallbackReason == -EUCLEAN && result.coreDegraded && get(2) == warps);
    sameActive(destination, expected);
    engine.flush();
    CHECK(engine.pending() == 0 && engine.gyroPeriodNs() == 0 && !engine.coreDegraded());
    CHECK(engine.processNext(destination.image, frame.timestampNs, result) == -ENODEV);
    CHECK(engine.configure(config) == 0 && !engine.coreDegraded());
    set(1, 0);
    CHECK(engine.enqueue(source.image, frame, frame.timestampNs) == 0);
    CHECK(engine.discard(frame.frameNumber + 1) == -EINVAL && engine.pending() == 1);
    CHECK(engine.discard(frame.frameNumber) == 0 && engine.pending() == 0);

    // Renderer keeps padded bytes intact, and cancellation cannot commit a partial mesh.
    const QCamera3MmsVstab::Vertex identity[] = {
        {-1, -1, 0}, {1, -1, 0}, {-1, 1, 0}, {1, 1, 0}};
    Buffer padded(8, 6, 4);
    CHECK(QCamera3MmsVstab::render(source.image, padded.image, identity, 2, 2) == 0);
    sameActive(padded, expected);
    for (uint32_t y = 0; y < padded.image.height; ++y)
        for (uint32_t x = padded.image.width; x < padded.image.yStride; ++x)
            CHECK(padded.pixels[y * padded.image.yStride + x] == 0xcc);
    const auto untouched = padded.pixels;
    std::atomic<bool> cancelled{true};
    CHECK(QCamera3MmsVstab::render(source.image, padded.image, identity, 2, 2, &cancelled) == -ECANCELED);
    CHECK(padded.pixels == untouched);

    // A native fallback starts with the resized real source, then refuses missing gyro honestly.
    QCamera3GyroVstab native;
    native.setEnabled(true);
    CHECK(QCamera3MmsVstab::renderPassthrough(source.image, padded.image) == 0);
    QCamera3GyroVstab::Options options;
    options.strength = 100; options.maxCropPercent = 8; options.windowNs = 33333333;
    options.timestampNs = 2000000000; options.focalLengthPx = 100;
    options.path = QCamera3GyroVstab::PATH_NATIVE; options.passthrough = 1;
    QCamera3GyroVstab::Result nativeResult;
    CHECK(native.process(padded.image, padded.image, options, nativeResult) == -ENODATA);
    CHECK(nativeResult.reason == QCamera3GyroVstab::REASON_NO_GYRO && nativeResult.applied == 0);
    sameActive(padded, expected);
    const float motion[] = {0.02f, 0.04f, 0.0f};
    for (int64_t timestamp = 1900000000; timestamp <= 2010000000; timestamp += 5000000)
        CHECK(native.addGyro(timestamp, motion) == 0);
    CHECK(native.process(padded.image, padded.image, options, nativeResult) == 0);
    CHECK(nativeResult.path == QCamera3GyroVstab::PATH_NATIVE && nativeResult.applied == 100);
    CHECK(nativeResult.reason == QCamera3GyroVstab::REASON_OK && nativeResult.gyroCount > 0);
    CHECK(padded.pixels != untouched);
    native.reset();
    CHECK(native.gyroSamples() == 0);
    dlclose(fake);
    puts("MMS profile/core ABI/retention/fallback/render host tests passed");
    return 0;
}
