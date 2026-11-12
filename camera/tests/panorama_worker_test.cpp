/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3PanoramaWorker.h"
#include "visidon_jnienv.h"
#include "QCamera3PanoramaGyro.h"
#include "fake_panorama_gyro_source.h"

#include <chrono>
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
#include <jpeglib.h>

using qcamera::QCamera3OppoPanorama;
using qcamera::QCamera3PanoramaWorker;
using Worker = QCamera3PanoramaWorker;
static int failures = 0;
#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #value); ++failures; } } while (0)

namespace {
using namespace std::chrono_literals;

bool ready(Worker &worker) {
    for (int i = 0; i < 5000; ++i) {
        const auto state = worker.stats().state;
        if (state != Worker::State::Initializing) return state == Worker::State::Capturing;
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

Worker::Config config(uint32_t width = 64, uint32_t height = 48) {
    Worker::Config config;
    config.panorama.previewWidth = width;
    config.panorama.previewHeight = height;
    config.panorama.outputWidth = width;
    config.panorama.outputHeight = height;
    config.panorama.canvasWidth = width * 3;
    config.panorama.canvasHeight = height;
    config.panorama.searchRadiusPx = 0;
    config.panorama.crop = true;
    config.minFrameIntervalNs = 0;
    return config;
}

struct Input {
    std::vector<uint8_t> bytes;
    Worker::Frame frame;
    Input(uint32_t width, uint32_t height, uint8_t luma = 180) {
        frame.width = width;
        frame.height = height;
        frame.yStride = width + 8;
        frame.uvStride = width + 16;
        frame.yOffset = 7;
        frame.uvOffset = frame.yOffset + size_t(frame.yStride) * height + 11;
        bytes.assign(frame.uvOffset + size_t(frame.uvStride) * height / 2, 0xee);
        for (uint32_t y = 0; y < height; ++y)
            memset(bytes.data() + frame.yOffset + size_t(y) * frame.yStride, luma, width);
        for (uint32_t y = 0; y < height / 2; ++y)
            memset(bytes.data() + frame.uvOffset + size_t(y) * frame.uvStride, 128, width);
        frame.data = bytes.data();
        frame.length = bytes.size();
    }
};

struct Completion {
    std::mutex mutex;
    std::condition_variable changed;
    int calls = 0;
    Worker::Result result;
    void accept(Worker::Result value) {
        std::lock_guard<std::mutex> lock(mutex);
        result = std::move(value);
        ++calls;
        changed.notify_all();
    }
    bool wait() {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, 5s, [this] { return calls != 0; });
    }
};

int decodedLuma(const std::vector<uint8_t> &jpeg, uint32_t width, uint32_t height) {
    jpeg_decompress_struct decoder = {};
    jpeg_error_mgr error;
    decoder.err = jpeg_std_error(&error);
    jpeg_create_decompress(&decoder);
    jpeg_mem_src(&decoder, jpeg.data(), jpeg.size());
    jpeg_read_header(&decoder, TRUE);
    CHECK(decoder.image_width == width && decoder.image_height == height);
    decoder.out_color_space = JCS_GRAYSCALE;
    jpeg_start_decompress(&decoder);
    std::vector<uint8_t> row(decoder.output_width);
    JSAMPROW pointer = row.data();
    jpeg_read_scanlines(&decoder, &pointer, 1);
    const int luma = row[width / 2];
    jpeg_abort_decompress(&decoder);
    jpeg_destroy_decompress(&decoder);
    return luma;
}

using Front = qcamera::QCamera3OppoVisidonPanorama;
using Jni = qcamera::VisidonJniEnv;
struct FrontObservation {
    int width = 0, height = 0, focal = 0, transposed = 0;
    std::vector<uint8_t> measured, added;
} frontObservation;
struct FrontEngine {
    std::vector<std::vector<uint8_t>> frames;
    std::vector<int32_t> displacements;
    uint32_t width = 0, height = 0;
    bool transposed = false;
};
long long frontInit(void *, void *, int width, int height, int focal, int transposed) {
    frontObservation = {width, height, focal, transposed, {}, {}};
    auto *engine = new FrontEngine;
    engine->width = uint32_t(width);
    engine->height = uint32_t(height);
    engine->transposed = transposed != 0;
    return reinterpret_cast<intptr_t>(engine);
}
int frontAdd(void *, void *, void *array, int width, int height, int displacement, long long handle) {
    auto *engine = reinterpret_cast<FrontEngine *>(handle);
    const auto *input = static_cast<Jni::ByteArrayIn *>(array);
    const auto *data = static_cast<const uint8_t *>(input->data);
    engine->frames.emplace_back(data, data + input->size);
    CHECK(width == int(engine->width) && height == int(engine->height));
    frontObservation.added.assign(data, data + input->size);
    engine->displacements.push_back(displacement);
    return 0;
}
void *frontMeasure(void *env, void *, void *array, int width, int height, float, float, long long handle) {
    const auto *engine = reinterpret_cast<FrontEngine *>(handle);
    const auto *input = static_cast<Jni::ByteArrayIn *>(array);
    const auto *data = static_cast<const uint8_t *>(input->data);
    CHECK(width == int(engine->width) && height == int(engine->height));
    frontObservation.measured.assign(data, data + input->size);
    const float position = (float(data[0]) - 100.0f) / 100.0f;
    auto *table = *static_cast<void ***>(env);
    using NewArray = void *(*)(void *, int);
    using Set = void (*)(void *, void *, int, int, const float *);
    void *output = reinterpret_cast<NewArray>(table[Jni::kNewFloatArray])(env, 4);
    const float values[4] = {engine->transposed ? 0.0f : position,
        engine->transposed ? position * engine->width / engine->height : 0.0f, 2.0f, 1.0f};
    reinterpret_cast<Set>(table[Jni::kSetFloatArrayRegion])(env, output, 0, 4, values);
    return output;
}
void *frontProcess(void *env, void *, long long handle) {
    auto *engine = reinterpret_cast<FrontEngine *>(handle);
    CHECK(engine->transposed);
    CHECK(engine->frames.size() == 3);
    CHECK(engine->displacements == std::vector<int32_t>({0, 16, 32}));
    auto *table = *static_cast<void ***>(env);
    using NewArray = void *(*)(void *, int);
    using Set = void (*)(void *, void *, int, int, const int32_t *);
    void *output = reinterpret_cast<NewArray>(table[Jni::kNewIntArray])(env, 2);
    const int32_t values[2] = {int32_t(engine->width * (engine->transposed ? 1 : 3)),
        int32_t(engine->height * (engine->transposed ? 3 : 1))};
    reinterpret_cast<Set>(table[Jni::kSetIntArrayRegion])(env, output, 0, 2, values);
    return output;
}
void *frontNv21(void *env, void *, long long handle) {
    auto *engine = reinterpret_cast<FrontEngine *>(handle);
    const uint32_t width = engine->width * (engine->transposed ? 1 : 3);
    const uint32_t height = engine->height * (engine->transposed ? 3 : 1);
    std::vector<uint8_t> bytes(size_t(width) * height * 3 / 2, 128);
    for (size_t n = 0; n < engine->frames.size(); ++n) {
        for (uint32_t y = 0; y < engine->height; ++y) {
            const size_t offset = engine->transposed ? size_t(n * engine->height + y) * width :
                    size_t(y) * width + n * engine->width;
            memcpy(bytes.data() + offset,
                    engine->frames[n].data() + size_t(y) * engine->width, engine->width);
        }
    }
    auto *table = *static_cast<void ***>(env);
    using NewArray = void *(*)(void *, int);
    using Set = void (*)(void *, void *, int, int, const uint8_t *);
    void *output = reinterpret_cast<NewArray>(table[Jni::kNewByteArray])(env, int(bytes.size()));
    reinterpret_cast<Set>(table[Jni::kSetByteArrayRegion])(env, output, 0,
            int(bytes.size()), bytes.data());
    return output;
}
void *frontUnusedJpeg(void *, void *, int, int, long long) { return nullptr; }
void frontRelease(void *, void *, long long handle) { delete reinterpret_cast<FrontEngine *>(handle); }
Front::EngineAbi frontAbi() {
    Front::EngineAbi api;
    api.init = frontInit;
    api.addYuvFrame = frontAdd;
    api.measureOverlap = frontMeasure;
    api.process = frontProcess;
    api.getNv21 = frontNv21;
    api.getJpeg = frontUnusedJpeg;
    api.release = frontRelease;
    return api;
}
void pendingDrained(Worker &worker) {
    for (int i = 0; i < 5000 && worker.stats().pendingFrames != 0; ++i)
        std::this_thread::sleep_for(1ms);
    CHECK(worker.stats().pendingFrames == 0);
}
void frontUsesMeasuredKeyframesAndSharedJpeg(int orientation) {
    Worker worker;
    const auto api = frontAbi();
    auto cfg = config();
    cfg.backend = Worker::Backend::FrontVisidon;
    cfg.focalPx = 100;
    cfg.testVisidonEngine = &api;
    cfg.panorama.orientationDeg = orientation;
    CHECK(worker.start(cfg) == Worker::OK);
    CHECK(ready(worker));
    const uint8_t luma[5] = {100, 100, 125, 150, 125};
    for (uint32_t n = 0; n < 5; ++n) {
        Input input(64, 48, luma[n]);
        input.frame.source = {20 + n, 123456000 + n};
        CHECK(worker.submit(input.frame) == Worker::OK);
        memset(input.bytes.data(), 0, input.bytes.size());
        pendingDrained(worker);
    }
    CHECK(worker.stats().acceptedFrames == 3);
    CHECK(worker.stats().droppedFrames == 2);
    Completion completion;
    CHECK(worker.requestJpeg(500, 55, 16384, [&](Worker::Result result) {
        completion.accept(std::move(result));
    }) == Worker::OK);
    CHECK(completion.wait());
    CHECK(completion.result.status == Worker::OK);
    CHECK(completion.result.failureReason == 0);
    const uint32_t outWidth = orientation == 90 ? 144 : 64;
    const uint32_t outHeight = orientation == 90 ? 64 : 144;
    CHECK(completion.result.width == outWidth && completion.result.height == outHeight);
    CHECK(completion.result.sources.size() == 3);
    if (completion.result.sources.size() == 3) {
        CHECK(completion.result.sources[0].frameNumber == 20);
        CHECK(completion.result.sources[1].frameNumber == 22);
        CHECK(completion.result.sources[2].timestampNs == 123456003);
    }
    CHECK(!completion.result.jpeg.empty());
    if (!completion.result.jpeg.empty()) {
        const int luma = decodedLuma(completion.result.jpeg, outWidth, outHeight);
        CHECK(orientation == 90 ? luma > 110 && luma < 145 : luma > 90 && luma < 110);
        CHECK(completion.result.jpeg[30] == 1 && completion.result.jpeg[31] == 0);
    }
    worker.cancelAndDrain();
    CHECK(worker.stats().pendingBytes == 0);
}

void frontNormalizesPaddedSourceAtPixelBoundary() {
    const auto api = frontAbi();
    Worker worker;
    struct Case { uint32_t width, height, outWidth, outHeight; int focal, outFocal; };
    const Case cases[] = {
        {1840, 1380, 1440, 1080, 1408, 1102},
        {1440, 1080, 1440, 1080, 1102, 1102},
        {1280, 960, 1280, 960, 980, 980},
        {1920, 1080, 1440, 808, 1470, 1103},
    };
    for (const auto &test : cases) {
        auto cfg = config(test.width, test.height);
        cfg.backend = Worker::Backend::FrontVisidon;
        cfg.focalPx = test.focal;
        cfg.testVisidonEngine = &api;
        CHECK(worker.start(cfg) == Worker::OK);
        CHECK(ready(worker));
        CHECK(frontObservation.width == int(test.outWidth));
        CHECK(frontObservation.height == int(test.outHeight));
        CHECK(frontObservation.focal == test.outFocal);
        CHECK(frontObservation.transposed == 1);
        Input input(test.width, test.height);
        auto pixel = [](uint32_t x, uint32_t y, int channel) -> uint8_t {
            if (channel == 0) return uint8_t(x / 16 + y / 32);
            return channel == 1 ? uint8_t(30 + x / 16) : uint8_t(210 - y / 16);
        };
        for (uint32_t y = 0; y < test.height; ++y)
            for (uint32_t x = 0; x < test.width; ++x)
                input.bytes[input.frame.yOffset + size_t(y) * input.frame.yStride + x] = pixel(x, y, 0);
        for (uint32_t y = 0; y < test.height / 2; ++y)
            for (uint32_t x = 0; x < test.width / 2; ++x)
                for (int c = 0; c < 2; ++c)
                    input.bytes[input.frame.uvOffset + size_t(y) * input.frame.uvStride + 2 * x + c] = pixel(x, y, c + 1);
        input.frame.source = {73, 123456789};
        CHECK(worker.submit(input.frame) == Worker::OK);
        memset(input.bytes.data(), 0xee, input.bytes.size());
        pendingDrained(worker);
        CHECK(worker.stats().acceptedFrames == 1);
        CHECK(frontObservation.measured == frontObservation.added);
        CHECK(frontObservation.measured.size() == size_t(test.outWidth) * test.outHeight * 3 / 2);
        const auto sources = worker.retainedSources();
        CHECK(sources.size() == 1);
        if (sources.size() == 1) CHECK(sources[0].frameNumber == 73 && sources[0].timestampNs == 123456789);
        for (int c = 0; c < 3; ++c) {
            const uint32_t sw = c == 0 ? test.width : test.width / 2;
            const uint32_t sh = c == 0 ? test.height : test.height / 2;
            const uint32_t dw = c == 0 ? test.outWidth : test.outWidth / 2;
            const uint32_t dh = c == 0 ? test.outHeight : test.outHeight / 2;
            for (uint32_t y : {0u, 1u, dh / 3, dh - 1}) {
                for (uint32_t x : {0u, 1u, dw / 3, dw - 1}) {
                    const double sx = std::clamp((x + 0.5) * sw / dw - 0.5, 0.0, double(sw - 1));
                    const double sy = std::clamp((y + 0.5) * sh / dh - 0.5, 0.0, double(sh - 1));
                    const uint32_t x0 = uint32_t(sx), y0 = uint32_t(sy);
                    const uint32_t x1 = std::min(x0 + 1, sw - 1), y1 = std::min(y0 + 1, sh - 1);
                    const double wx = sx - x0, wy = sy - y0;
                    const double expected = (pixel(x0, y0, c) * (1 - wx) + pixel(x1, y0, c) * wx) * (1 - wy) +
                            (pixel(x0, y1, c) * (1 - wx) + pixel(x1, y1, c) * wx) * wy;
                    const size_t offset = c == 0 ? size_t(y) * dw + x :
                            size_t(test.outWidth) * test.outHeight + size_t(y) * test.outWidth + 2 * x + c - 1;
                    CHECK(std::abs(frontObservation.measured[offset] - expected) <= 1.0);
                }
            }
        }
        worker.cancelAndDrain();
        CHECK(worker.stats().pendingBytes == 0);
    }
}
void frontStationaryFramesFailWithoutImageAndCanRestart() {
    Worker worker;
    const auto api = frontAbi();
    auto cfg = config();
    cfg.backend = Worker::Backend::FrontVisidon;
    cfg.focalPx = 100;
    cfg.testVisidonEngine = &api;
    CHECK(worker.start(cfg) == Worker::OK);
    CHECK(ready(worker));
    for (uint32_t n = 0; n < 4; ++n) {
        Input input(64, 48, 100);
        input.frame.source = {n, int64_t(n + 1)};
        CHECK(worker.submit(input.frame) == Worker::OK);
        pendingDrained(worker);
    }
    Completion completion;
    CHECK(worker.requestJpeg(501, 95, 8192, [&](Worker::Result result) {
        completion.accept(std::move(result));
    }) == Worker::OK);
    CHECK(completion.wait());
    CHECK(completion.result.status == QCamera3OppoPanorama::E_NOFRAME);
    CHECK(completion.result.failureReason == 3);
    CHECK(completion.result.jpeg.empty());
    worker.cancelAndDrain();
    CHECK(worker.start(cfg) == Worker::OK);
    CHECK(ready(worker));
    CHECK(frontObservation.transposed == 1);
    worker.cancelAndDrain();
}

void copiedPlanesAndSourcesReachJpeg() {
    Worker worker;
    auto cfg = config();
    cfg.panorama.orientationDeg = 90;
    CHECK(worker.start(cfg) == Worker::OK);
    CHECK(ready(worker));
    for (uint32_t n = 0; n < 2; ++n) {
        Input input(64, 48);
        input.frame.source.frameNumber = 70 + n;
        input.frame.source.timestampNs = 123000000 + n;
        CHECK(worker.submit(input.frame) == Worker::OK);
        // The caller returns/reuses the source immediately after submit.
        memset(input.bytes.data(), 0, input.bytes.size());
    }
    Completion completion;
    CHECK(worker.requestJpeg(901, 95, 8192, [&](Worker::Result result) {
        completion.accept(std::move(result));
    }) == Worker::OK);
    CHECK(worker.requestJpeg(902, 95, 8192, [](Worker::Result) {}) == Worker::E_BUSY);
    CHECK(completion.wait());
    CHECK(completion.calls == 1);
    CHECK(completion.result.status == Worker::OK);
    CHECK(completion.result.requestFrame == 901);
    CHECK(completion.result.width == 48 && completion.result.height == 64);
    CHECK(completion.result.sources.size() == 2);
    const auto retained = worker.retainedSources();
    CHECK(retained.size() == 2);
    if (retained.size() == 2) {
        CHECK(retained[0].frameNumber == 70);
        CHECK(retained[1].timestampNs == 123000001);
    }
    if (completion.result.sources.size() == 2) {
        CHECK(completion.result.sources[0].frameNumber == 70);
        CHECK(completion.result.sources[1].timestampNs == 123000001);
    }
    if (!completion.result.jpeg.empty()) {
        CHECK(decodedLuma(completion.result.jpeg, 48, 64) > 150);
        // TIFF count=1, EXIF orientation=1: pixels were already rotated once.
        const auto &jpeg = completion.result.jpeg;
        CHECK(jpeg[26] == 1 && jpeg[27] == 0 && jpeg[28] == 0 && jpeg[29] == 0);
        CHECK(jpeg[30] == 1 && jpeg[31] == 0);
    }
    worker.cancelAndDrain();
    CHECK(worker.stats().pendingBytes == 0);
}

void inputValidationIntervalAndQueueBounds() {
    Worker worker;
    auto cfg = config(640, 480);
    cfg.maxPendingFrames = 1;
    cfg.maxPendingBytes = size_t(640) * 480 * 3 / 2;
    cfg.minFrameIntervalNs = 250000000;
    CHECK(worker.start(cfg) == Worker::OK);
    CHECK(ready(worker));
    Input input(640, 480);
    input.frame.source.timestampNs = 1000000000;
    Worker::Frame invalid = input.frame;
    invalid.length = invalid.uvOffset + 1;
    CHECK(worker.submit(invalid) == QCamera3OppoPanorama::E_PARAM);
    invalid = input.frame;
    invalid.yOffset = SIZE_MAX;
    CHECK(worker.submit(invalid) == QCamera3OppoPanorama::E_PARAM);
    CHECK(worker.submit(input.frame) == Worker::OK);
    input.frame.source.timestampNs += 1000000;
    CHECK(worker.submit(input.frame) == Worker::E_BUSY);
    for (int i = 0; i < 20; ++i) {
        input.frame.source.timestampNs += 250000000;
        const int32_t status = worker.submit(input.frame);
        CHECK(status == Worker::OK || status == Worker::E_BUSY);
        const auto stats = worker.stats();
        CHECK(stats.pendingFrames <= cfg.maxPendingFrames);
        CHECK(stats.pendingBytes <= cfg.maxPendingBytes);
    }
    worker.cancelAndDrain();
    CHECK(worker.stats().state == Worker::State::Canceled);
    CHECK(worker.stats().pendingFrames == 0 && worker.stats().pendingBytes == 0);
    CHECK(worker.stats().droppedFrames > 0);
    CHECK(worker.stats().peakPendingBytes <= cfg.maxPendingBytes);
    CHECK(worker.submit(input.frame) == Worker::E_BUSY);
}

void tooFewFramesCompleteWithFailureAndRestartWorks() {
    Worker worker;
    CHECK(worker.start(config()) == Worker::OK);
    CHECK(ready(worker));
    Completion completion;
    CHECK(worker.requestJpeg(8, 95, 8192, [&](Worker::Result result) {
        completion.accept(std::move(result));
    }) == Worker::OK);
    CHECK(completion.wait());
    worker.cancelAndDrain();
    CHECK(completion.calls == 1);
    CHECK(completion.result.status == QCamera3OppoPanorama::E_NOFRAME);
    CHECK(completion.result.jpeg.empty());
    CHECK(worker.start(config()) == Worker::OK);
    CHECK(ready(worker));
    worker.cancelAndDrain();
}

void cancellationCompletesAcceptedRequestExactlyOnce() {
    Worker worker;
    auto cfg = config(1024, 768);
    CHECK(worker.start(cfg) == Worker::OK);
    CHECK(ready(worker));
    for (uint32_t n = 0; n < 2; ++n) {
        Input input(1024, 768);
        input.frame.source = {n, int64_t(n + 1)};
        CHECK(worker.submit(input.frame) == Worker::OK);
    }
    Completion completion;
    CHECK(worker.requestJpeg(42, 95, 2000000, [&](Worker::Result result) {
        completion.accept(std::move(result));
    }) == Worker::OK);
    worker.cancelAndDrain();
    CHECK(completion.calls == 1);
    CHECK(completion.result.requestFrame == 42);
    // Completion may have won before cancel; a canceled result never contains pixels.
    CHECK(completion.result.status == Worker::OK ||
            completion.result.status == QCamera3OppoPanorama::E_CANCELED);
    if (completion.result.status == QCamera3OppoPanorama::E_CANCELED)
        CHECK(completion.result.jpeg.empty());
    CHECK(worker.stats().pendingBytes == 0);
}

void cancelBarrierWaitsForCompletionCallback() {
    Worker worker;
    CHECK(worker.start(config()) == Worker::OK);
    CHECK(ready(worker));
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool release = false;
    bool drained = false;
    CHECK(worker.requestJpeg(99, 95, 8192, [&](Worker::Result) {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        changed.notify_all();
        changed.wait(lock, [&] { return release; });
    }) == Worker::OK);
    {
        std::unique_lock<std::mutex> lock(mutex);
        CHECK(changed.wait_for(lock, 5s, [&] { return entered; }));
    }
    std::thread flush([&] {
        worker.cancelAndDrain();
        std::lock_guard<std::mutex> lock(mutex);
        drained = true;
    });
    {
        std::lock_guard<std::mutex> lock(mutex);
        CHECK(!drained);
        release = true;
        changed.notify_all();
    }
    flush.join();
    CHECK(drained);
}

void outputMemoryBudgetIsCheckedBeforeCapture() {
    Worker worker;
    auto cfg = config();
    cfg.maxOutputBytes = 1;
    CHECK(worker.start(cfg) == Worker::OK);
    CHECK(!ready(worker));
    CHECK(worker.stats().state == Worker::State::Failed);
    CHECK(worker.stats().lastStatus == QCamera3OppoPanorama::E_NOMEM);
    worker.cancelAndDrain();
}

void gyroIntegratesOnlyTheSourceFrameInterval() {
    qcamera::QCamera3PanoramaGyro timeline;
    const float velocity[3] = {float(M_PI / 2), float(M_PI), 0.0f};
    const float zero[3] = {};
    for (int mount : {0, 90, 180, 270}) {
        timeline.reset(mount);
        CHECK(timeline.add(1000000000, zero) == 0);
        CHECK(timeline.add(1020000000, velocity) == 0);
        CHECK(timeline.add(1040000000, velocity) == 0);
        auto result = timeline.interval(1010000000, 1030000000);
        CHECK(result.valid && result.timestampUs == 1030000);
        const float pitch = 1.575f, yaw = 3.15f;
        const float expectedX = mount == 0 ? -yaw : mount == 90 ? -pitch : mount == 180 ? yaw : pitch;
        const float expectedY = mount == 0 ? -pitch : mount == 90 ? yaw : mount == 180 ? pitch : -yaw;
        CHECK(std::abs(result.angleXDeg - expectedX) < 0.00001f);
        CHECK(std::abs(result.angleYDeg - expectedY) < 0.00001f);
        CHECK(!timeline.interval(999999999, 1030000000).valid);
        CHECK(!timeline.interval(1010000000, 1040000001).valid);
        CHECK(!timeline.interval(1030000000, 1010000000).valid);
        CHECK(timeline.add(1040000000, zero) == -EINVAL);
        CHECK(timeline.add(1100000000, velocity) == 0);
        CHECK(!timeline.interval(1030000000, 1090000000).valid);
    }
    timeline.reset(0);
    for (int64_t n = 1; n <= 600; ++n) CHECK(timeline.add(n * 5000000, velocity) == 0);
    CHECK(!timeline.interval(5000000, 10000000).valid);
    CHECK(timeline.interval(2990000000, 3000000000).valid);
}

void rearMatchesGyroAndDrainsFeedAcrossFinishAndCancel() {
    qcamera::panoramaTestGyroReset();
    Worker worker;
    auto cfg = config();
    cfg.useGyro = true;
    cfg.sensorMountAngle = 90;
    cfg.panorama.pxPerDeg = 10.0f;
    cfg.panorama.centerCanvas = true;
    cfg.panorama.preserveCropSize = true;
    CHECK(worker.start(cfg) == Worker::OK);
    CHECK(ready(worker));
    CHECK(qcamera::panoramaTestGyroStarts() == 1);
    CHECK(qcamera::panoramaTestGyroPeriod() == 5000000);
    const float velocity[3] = {float(100 * M_PI / 180), 0.0f, 0.0f};
    for (int64_t time = 995000000; time <= 1025000000; time += 5000000)
        CHECK(qcamera::panoramaTestGyroEmit(time, velocity) == 0);
    for (uint32_t n = 0; n < 2; ++n) {
        Input input(64, 48);
        input.frame.source = {n + 70, 1000000000 + n * 20000000};
        CHECK(worker.submit(input.frame) == Worker::OK);
        pendingDrained(worker);
    }
    CHECK(worker.stats().gyroFeedStarted && worker.stats().gyroFeedStatus == 0);
    CHECK(worker.stats().gyroMatchedFrames == 1 && worker.stats().gyroMissingFrames == 0);
    Completion completion;
    CHECK(worker.requestJpeg(777, 95, 16384, [&](Worker::Result result) {
        completion.accept(std::move(result));
    }) == Worker::OK);
    CHECK(completion.wait());
    CHECK(completion.result.status == Worker::OK);
    CHECK(completion.result.width == 84 && completion.result.height == 48);
    CHECK(completion.result.sources.size() == 2);
    if (!completion.result.jpeg.empty()) CHECK(decodedLuma(completion.result.jpeg, 84, 48) > 150);
    CHECK(qcamera::panoramaTestGyroStops() == 1);
    CHECK(qcamera::panoramaTestGyroEmit(1030000000, velocity) == -ENODEV);
    worker.cancelAndDrain();
    CHECK(worker.start(cfg) == Worker::OK);
    CHECK(ready(worker));
    worker.cancelAndDrain();
    CHECK(qcamera::panoramaTestGyroStarts() == 2 && qcamera::panoramaTestGyroStops() == 2);
}

void missingGyroRemainsAnObservableImageOnlyFallback() {
    qcamera::panoramaTestGyroReset(-ENODEV);
    Worker worker;
    auto cfg = config();
    cfg.useGyro = true;
    cfg.panorama.pxPerDeg = 10.0f;
    CHECK(worker.start(cfg) == Worker::OK);
    CHECK(ready(worker));
    CHECK(!worker.stats().gyroFeedStarted && worker.stats().gyroFeedStatus == -ENODEV);
    for (uint32_t n = 0; n < 2; ++n) {
        Input input(64, 48);
        input.frame.source = {n, 1000000000 + n * 20000000};
        CHECK(worker.submit(input.frame) == Worker::OK);
        pendingDrained(worker);
    }
    CHECK(worker.stats().gyroMatchedFrames == 0 && worker.stats().gyroMissingFrames == 1);
    Completion completion;
    CHECK(worker.requestJpeg(778, 95, 16384, [&](Worker::Result result) {
        completion.accept(std::move(result));
    }) == Worker::OK);
    CHECK(completion.wait());
    CHECK(completion.result.status == Worker::OK && !completion.result.jpeg.empty());
    worker.cancelAndDrain();
    qcamera::panoramaTestGyroReset();
}

void frontBackendErrorCompletesWithItsOriginalFailure() {
    Worker worker;
    auto api = frontAbi();
    api.addYuvFrame = [](void *, void *, void *, int, int, int, long long) { return -1; };
    auto cfg = config();
    cfg.backend = Worker::Backend::FrontVisidon;
    cfg.focalPx = 100;
    cfg.testVisidonEngine = &api;
    CHECK(worker.start(cfg) == Worker::OK);
    CHECK(ready(worker));
    Input input(64, 48, 100);
    input.frame.source = {3, 1000000000};
    CHECK(worker.submit(input.frame) == Worker::OK);
    pendingDrained(worker);
    CHECK(worker.stats().state == Worker::State::Failed);
    CHECK(worker.stats().lastStatus == Worker::E_BACKEND);
    Completion completion;
    CHECK(worker.requestJpeg(779, 95, 16384, [&](Worker::Result result) {
        completion.accept(std::move(result));
    }) == Worker::OK);
    CHECK(completion.wait());
    CHECK(completion.calls == 1 && completion.result.status == Worker::E_BACKEND);
    CHECK(completion.result.failureReason == 13 && completion.result.jpeg.empty());
    worker.cancelAndDrain();
}
}

int main() {
    frontUsesMeasuredKeyframesAndSharedJpeg(90);
    frontUsesMeasuredKeyframesAndSharedJpeg(0);
    frontNormalizesPaddedSourceAtPixelBoundary();
    frontStationaryFramesFailWithoutImageAndCanRestart();
    copiedPlanesAndSourcesReachJpeg();
    inputValidationIntervalAndQueueBounds();
    tooFewFramesCompleteWithFailureAndRestartWorks();
    cancellationCompletesAcceptedRequestExactlyOnce();
    cancelBarrierWaitsForCompletionCallback();
    outputMemoryBudgetIsCheckedBeforeCapture();
    gyroIntegratesOnlyTheSourceFrameInterval();
    rearMatchesGyroAndDrainsFeedAcrossFinishAndCancel();
    missingGyroRemainsAnObservableImageOnlyFallback();
    frontBackendErrorCompletesWithItsOriginalFailure();
    if (failures) return 1;
    puts("panorama worker tests: PASS");
    return 0;
}
