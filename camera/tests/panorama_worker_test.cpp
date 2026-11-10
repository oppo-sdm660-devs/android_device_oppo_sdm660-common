/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3PanoramaWorker.h"

#include <chrono>
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
}

int main() {
    copiedPlanesAndSourcesReachJpeg();
    inputValidationIntervalAndQueueBounds();
    tooFewFramesCompleteWithFailureAndRestartWorks();
    cancellationCompletesAcceptedRequestExactlyOnce();
    cancelBarrierWaitsForCompletionCallback();
    outputMemoryBudgetIsCheckedBeforeCapture();
    if (failures) return 1;
    puts("panorama worker tests: PASS");
    return 0;
}
