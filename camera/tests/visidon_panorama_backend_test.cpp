/* SPDX-License-Identifier: BSD-3-Clause */
// Host tests for the front Visidon selfie-panorama backend (task-156).
// Pure C++: the vendor library is replaced by a mock EngineAbi; the fake
// JNIEnv shim is the real one from HAL3, so the ABI indirection is exercised.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <vector>

#include "QCamera3OppoVisidonPanorama.h"
#include "visidon_jnienv.h"

using qcamera::QCamera3OppoVisidonPanorama;
using qcamera::VisidonJniEnv;

static int g_failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
            g_failures++;                                                           \
        }                                                                           \
    } while (0)

namespace {

struct Mock {
    int inits = 0;
    int adds = 0;
    int releases = 0;
    int cancels = 0;
    int addRc = 0;
    bool failProcess = false;
    bool triggerUnsupportedSlot = false;
    int lastRotation = -1;
    int lastAddWidth = 0;
    int lastAddHeight = 0;
    uint32_t outW = 64;
    uint32_t outH = 44;
    std::vector<uint8_t> lastFrame;
};

Mock g_mock;

void *arrayFromSlot(void *env, uint32_t slot, int length) {
    auto *table = *static_cast<void ***>(env);
    using NewArrayFn = void *(*)(void *, int);
    return reinterpret_cast<NewArrayFn>(table[slot])(env, length);
}

void setBytes(void *env, void *array, int length, const uint8_t *data) {
    auto *table = *static_cast<void ***>(env);
    using SetFn = void (*)(void *, void *, int, int, const uint8_t *);
    reinterpret_cast<SetFn>(table[VisidonJniEnv::kSetByteArrayRegion])(env, array, 0,
            length, data);
}

void setInts(void *env, void *array, int length, const int32_t *data) {
    auto *table = *static_cast<void ***>(env);
    using SetFn = void (*)(void *, void *, int, int, const int32_t *);
    reinterpret_cast<SetFn>(table[VisidonJniEnv::kSetIntArrayRegion])(env, array, 0,
            length, data);
}

long long mockInit(void *, void *, int, int, int, int) {
    g_mock.inits++;
    return 0x1234;
}

int mockAddYuv(void *env, void *, void *array, int width, int height, int, long long) {
    g_mock.adds++;
    g_mock.lastAddWidth = width;
    g_mock.lastAddHeight = height;
    auto *in = static_cast<VisidonJniEnv::ByteArrayIn *>(array);
    if (in == nullptr || in->data == nullptr) return 1;
    g_mock.lastFrame.assign(static_cast<const uint8_t *>(in->data),
            static_cast<const uint8_t *>(in->data) + in->size);
    (void)env;
    return g_mock.addRc;
}

void *mockProcess(void *env, void *, long long) {
    if (g_mock.triggerUnsupportedSlot) {
        auto *table = *static_cast<void ***>(env);
        using UnsupportedFn = void (*)(void *, void *, void *);
        reinterpret_cast<UnsupportedFn>(table[6])(env, nullptr, nullptr); // FindClass
    }
    if (g_mock.failProcess) return nullptr;
    void *array = arrayFromSlot(env, VisidonJniEnv::kNewIntArray, 2);
    const int32_t dims[2] = {int32_t(g_mock.outW), int32_t(g_mock.outH)};
    setInts(env, array, 2, dims);
    return array;
}

void *mockNv21(void *env, void *, long long) {
    const size_t len = size_t(g_mock.outW) * g_mock.outH * 3 / 2;
    void *array = arrayFromSlot(env, VisidonJniEnv::kNewByteArray, int(len));
    std::vector<uint8_t> data(len, 0x5a);
    setBytes(env, array, int(len), data.data());
    return array;
}

void *mockJpeg(void *env, void *, int width, int rotation, long long) {
    (void)width;
    g_mock.lastRotation = rotation;
    uint8_t jpeg[100];
    memset(jpeg, 0x2a, sizeof(jpeg));
    jpeg[0] = 0xff;
    jpeg[1] = 0xd8;
    void *array = arrayFromSlot(env, VisidonJniEnv::kNewByteArray, int(sizeof(jpeg)));
    setBytes(env, array, int(sizeof(jpeg)), jpeg);
    return array;
}

void mockCancel(void *, void *, long long) { g_mock.cancels++; }
void mockRelease(void *, void *, long long) { g_mock.releases++; }

QCamera3OppoVisidonPanorama::EngineAbi abi() {
    QCamera3OppoVisidonPanorama::EngineAbi api;
    api.init = mockInit;
    api.addYuvFrame = mockAddYuv;
    api.process = mockProcess;
    api.getNv21 = mockNv21;
    api.getJpeg = mockJpeg;
    api.cancelProcessing = mockCancel;
    api.release = mockRelease;
    return api;
}

QCamera3OppoVisidonPanorama::Config baseConfig(const QCamera3OppoVisidonPanorama::EngineAbi &api) {
    QCamera3OppoVisidonPanorama::Config config;
    config.previewWidth = 64;
    config.previewHeight = 48;
    config.focalPx = 100;
    config.rotationDeg = 0;
    config.minFrames = 3;
    config.maxFrames = 3;
    config.testEngine = &api;
    return config;
}

QCamera3OppoVisidonPanorama::Frame makeFrame(uint32_t width, uint32_t height,
        uint32_t yStride, uint32_t uvStride, std::vector<uint8_t> &y,
        std::vector<uint8_t> &uv) {
    y.assign(size_t(yStride) * height, 0x11);
    uv.assign(size_t(uvStride) * (height / 2), 0x22);
    for (uint32_t row = 0; row < height; row++)
        for (uint32_t x = 0; x < width; x++) y[size_t(row) * yStride + x] = uint8_t(x);
    QCamera3OppoVisidonPanorama::Frame frame;
    frame.y = y.data();
    frame.uv = uv.data();
    frame.width = width;
    frame.height = height;
    frame.yStride = yStride;
    frame.uvStride = uvStride;
    return frame;
}

}  // namespace

int main() {
    const auto api = abi();
    std::vector<uint8_t> y, uv;

    // Parameter validation.
    {
        QCamera3OppoVisidonPanorama backend;
        auto config = baseConfig(api);
        config.rotationDeg = 45;
        CHECK(backend.init(config) == QCamera3OppoVisidonPanorama::E_PARAM);
        config = baseConfig(api);
        config.minFrames = 2;
        CHECK(backend.init(config) == QCamera3OppoVisidonPanorama::E_PARAM);
    }

    // Full pipeline: padded strides, 3 accepted frames, process, JPEG + NV21.
    {
        QCamera3OppoVisidonPanorama backend;
        auto config = baseConfig(api);
        CHECK(backend.init(config) == QCamera3OppoVisidonPanorama::OK);
        CHECK(backend.state() == QCamera3OppoVisidonPanorama::State::Capturing);
        auto frame = makeFrame(64, 48, 80, 80, y, uv);
        for (int i = 0; i < 3; i++) CHECK(backend.addFrame(frame) == QCamera3OppoVisidonPanorama::OK);
        CHECK(backend.acceptedFrames() == 3);
        CHECK(backend.addFrame(frame) == QCamera3OppoVisidonPanorama::E_LIMIT);
        CHECK(g_mock.lastFrame.size() == 64u * 48u * 3 / 2);
        for (uint32_t x = 0; x < 64; x++) CHECK(g_mock.lastFrame[x] == uint8_t(x));
        CHECK(g_mock.lastFrame[64 * 48] == 0x22);
        uint32_t outW = 0, outH = 0;
        CHECK(backend.process(&outW, &outH) == QCamera3OppoVisidonPanorama::OK);
        CHECK(outW == 64 && outH == 44);

        std::vector<uint8_t> jpeg(4);
        QCamera3OppoVisidonPanorama::Image image{jpeg.data(), jpeg.size(), 0, 0, 0};
        CHECK(backend.captureJpeg(&image) == QCamera3OppoVisidonPanorama::E_NOMEM);
        jpeg.assign(256, 0);
        image = {jpeg.data(), jpeg.size(), 0, 0, 0};
        CHECK(backend.captureJpeg(&image) == QCamera3OppoVisidonPanorama::OK);
        CHECK(image.length == 100);
        CHECK(image.width == 64 && image.height == 44);
        CHECK(image.data[0] == 0xff && image.data[1] == 0xd8);
        CHECK(g_mock.lastRotation == 0);

        backend.deinit();
        CHECK(backend.state() == QCamera3OppoVisidonPanorama::State::Idle);
    }

    // NV21 capture path (same 3 frames).
    {
        QCamera3OppoVisidonPanorama backend;
        CHECK(backend.init(baseConfig(api)) == QCamera3OppoVisidonPanorama::OK);
        auto frame = makeFrame(64, 48, 64, 64, y, uv);
        for (int i = 0; i < 3; i++) CHECK(backend.addFrame(frame) == QCamera3OppoVisidonPanorama::OK);
        std::vector<uint8_t> nv21(64 * 44 * 3 / 2);
        QCamera3OppoVisidonPanorama::Image nv{nv21.data(), nv21.size(), 0, 0, 0};
        CHECK(backend.captureNv21(&nv) == QCamera3OppoVisidonPanorama::OK);
        CHECK(nv.length == nv21.size());
        CHECK(nv.width == 64 && nv.height == 44);
        CHECK(nv.data[0] == 0x5a);
        CHECK(backend.state() == QCamera3OppoVisidonPanorama::State::Ready);
        backend.deinit();
    }

    // Rotation 90 swaps JPEG dimensions and is forwarded to the engine.
    {
        QCamera3OppoVisidonPanorama backend;
        auto config = baseConfig(api);
        config.rotationDeg = 90;
        CHECK(backend.init(config) == QCamera3OppoVisidonPanorama::OK);
        auto frame = makeFrame(64, 48, 64, 64, y, uv);
        for (int i = 0; i < 3; i++) CHECK(backend.addFrame(frame) == QCamera3OppoVisidonPanorama::OK);
        std::vector<uint8_t> jpeg(256);
        QCamera3OppoVisidonPanorama::Image image{jpeg.data(), jpeg.size(), 0, 0, 0};
        CHECK(backend.captureJpeg(&image) == QCamera3OppoVisidonPanorama::OK);
        CHECK(g_mock.lastRotation == 90);
        CHECK(image.width == 44 && image.height == 64);
        backend.deinit();
    }

    // Too few frames at capture fails closed (and no image is produced).
    {
        QCamera3OppoVisidonPanorama backend;
        CHECK(backend.init(baseConfig(api)) == QCamera3OppoVisidonPanorama::OK);
        auto frame = makeFrame(64, 48, 64, 64, y, uv);
        CHECK(backend.addFrame(frame) == QCamera3OppoVisidonPanorama::OK);
        CHECK(backend.addFrame(frame) == QCamera3OppoVisidonPanorama::OK);
        std::vector<uint8_t> jpeg(256, 0x7f);
        QCamera3OppoVisidonPanorama::Image image{jpeg.data(), jpeg.size(), 0, 0, 0};
        CHECK(backend.captureJpeg(&image) == QCamera3OppoVisidonPanorama::E_FRAMES);
        CHECK(image.length == 0);
        CHECK(backend.state() == QCamera3OppoVisidonPanorama::State::Failed);
        CHECK(backend.addFrame(frame) == QCamera3OppoVisidonPanorama::E_STATE);
        backend.deinit();
    }

    // Engine errors fail closed with the documented HAL reason mapping.
    {
        QCamera3OppoVisidonPanorama backend;
        g_mock.failProcess = true;
        CHECK(backend.init(baseConfig(api)) == QCamera3OppoVisidonPanorama::OK);
        auto frame = makeFrame(64, 48, 64, 64, y, uv);
        for (int i = 0; i < 3; i++) CHECK(backend.addFrame(frame) == QCamera3OppoVisidonPanorama::OK);
        uint32_t outW = 0, outH = 0;
        CHECK(backend.process(&outW, &outH) == QCamera3OppoVisidonPanorama::E_INTERNAL);
        g_mock.failProcess = false;
        backend.deinit();
    }
    {
        QCamera3OppoVisidonPanorama backend;
        g_mock.triggerUnsupportedSlot = true;
        CHECK(backend.init(baseConfig(api)) == QCamera3OppoVisidonPanorama::OK);
        auto frame = makeFrame(64, 48, 64, 64, y, uv);
        for (int i = 0; i < 3; i++) CHECK(backend.addFrame(frame) == QCamera3OppoVisidonPanorama::OK);
        uint32_t outW = 0, outH = 0;
        CHECK(backend.process(&outW, &outH) == QCamera3OppoVisidonPanorama::E_INTERNAL);
        g_mock.triggerUnsupportedSlot = false;
        backend.deinit();
    }

    // Cancel releases the engine and blocks further frame feeding.
    {
        QCamera3OppoVisidonPanorama backend;
        const int releasesBefore = g_mock.releases;
        CHECK(backend.init(baseConfig(api)) == QCamera3OppoVisidonPanorama::OK);
        CHECK(backend.cancel() == QCamera3OppoVisidonPanorama::OK);
        CHECK(backend.state() == QCamera3OppoVisidonPanorama::State::Canceled);
        CHECK(g_mock.releases > releasesBefore);
        auto frame = makeFrame(64, 48, 64, 64, y, uv);
        CHECK(backend.addFrame(frame) == QCamera3OppoVisidonPanorama::E_STATE);
        backend.deinit();
        backend.deinit(); // idempotent
    }

    // Failure-reason mapping (task-153 section 4).
    CHECK(QCamera3OppoVisidonPanorama::halFailureReason(QCamera3OppoVisidonPanorama::OK) == 0);
    CHECK(QCamera3OppoVisidonPanorama::halFailureReason(
            QCamera3OppoVisidonPanorama::E_LOAD) == 1);
    CHECK(QCamera3OppoVisidonPanorama::halFailureReason(
            QCamera3OppoVisidonPanorama::E_FRAMES) == 3);
    CHECK(QCamera3OppoVisidonPanorama::halFailureReason(
            QCamera3OppoVisidonPanorama::E_PARAM) == 7);
    CHECK(QCamera3OppoVisidonPanorama::halFailureReason(
            QCamera3OppoVisidonPanorama::E_ENCODE) == 8);
    CHECK(QCamera3OppoVisidonPanorama::halFailureReason(
            QCamera3OppoVisidonPanorama::E_JPEG) == 9);
    CHECK(QCamera3OppoVisidonPanorama::halFailureReason(
            QCamera3OppoVisidonPanorama::E_NOMEM) == 10);
    CHECK(QCamera3OppoVisidonPanorama::halFailureReason(
            QCamera3OppoVisidonPanorama::E_CANCELED) == 12);
    CHECK(QCamera3OppoVisidonPanorama::halFailureReason(
            QCamera3OppoVisidonPanorama::E_INTERNAL) == 13);

    printf("visidon panorama backend: %s (%d failures)\n",
            g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
