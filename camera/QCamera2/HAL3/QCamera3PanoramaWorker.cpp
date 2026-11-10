/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3PanoramaWorker.h"
#include "QCamera3PanoramaJpeg.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace qcamera {
namespace {
bool planeFits(size_t offset, uint32_t stride, uint32_t width, uint32_t rows, size_t length) {
    if (offset > length || stride < width || rows == 0) return false;
    const size_t available = length - offset;
    return width <= available && size_t(rows - 1) <= (available - width) / stride;
}
}

QCamera3PanoramaWorker::QCamera3PanoramaWorker() {
    thread_ = std::thread(&QCamera3PanoramaWorker::run, this);
}

QCamera3PanoramaWorker::~QCamera3PanoramaWorker() {
    cancelAndDrain();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        wake_.notify_one();
    }
    thread_.join();
}

int32_t QCamera3PanoramaWorker::start(const Config &config) {
    const size_t bytes = QCamera3OppoPanorama::nv21Size(config.panorama.previewWidth,
            config.panorama.previewHeight, config.panorama.previewWidth,
            config.panorama.previewWidth);
    if (bytes == 0 || config.maxPendingFrames == 0 || bytes > config.maxPendingBytes ||
            config.maxOutputBytes == 0 || config.minFrameIntervalNs < 0) {
        return QCamera3OppoPanorama::E_PARAM;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (working_ || initRequested_ || finishRequested_ || cancelRequested_ ||
            stats_.state == State::Capturing || stats_.state == State::Finalizing) return E_BUSY;
    config_ = config;
    stats_ = Stats();
    stats_.state = State::Initializing;
    sources_.clear();
    hasEnqueuedTimestamp_ = false;
    initRequested_ = true;
    wake_.notify_one();
    return OK;
}

bool QCamera3PanoramaWorker::validFrame(const Frame &frame) {
    return frame.data != nullptr && frame.width > 0 && frame.height > 0 &&
            (frame.width & 1u) == 0 && (frame.height & 1u) == 0 &&
            frame.source.timestampNs >= 0 &&
            planeFits(frame.yOffset, frame.yStride, frame.width, frame.height, frame.length) &&
            planeFits(frame.uvOffset, frame.uvStride, frame.width, frame.height / 2, frame.length);
}

int32_t QCamera3PanoramaWorker::submit(const Frame &frame) {
    if (!validFrame(frame)) return QCamera3OppoPanorama::E_PARAM;
    std::lock_guard<std::mutex> lock(mutex_);
    if (stats_.state != State::Capturing || cancelRequested_) return E_BUSY;
    if (frame.width != config_.panorama.previewWidth || frame.height != config_.panorama.previewHeight) {
        return QCamera3OppoPanorama::E_PARAM;
    }
    const size_t bytes = size_t(frame.width) * frame.height * 3 / 2;
    if (stats_.pendingFrames >= config_.maxPendingFrames ||
            bytes > config_.maxPendingBytes - stats_.pendingBytes ||
            (hasEnqueuedTimestamp_ && (frame.source.timestampNs <= lastEnqueuedTimestampNs_ ||
                frame.source.timestampNs - lastEnqueuedTimestampNs_ < config_.minFrameIntervalNs))) {
        ++stats_.droppedFrames;
        return E_BUSY;
    }
    OwnedFrame owned;
    owned.bytes.resize(bytes);
    for (uint32_t row = 0; row < frame.height; ++row) {
        memcpy(owned.bytes.data() + size_t(row) * frame.width,
                frame.data + frame.yOffset + size_t(row) * frame.yStride, frame.width);
    }
    for (uint32_t row = 0; row < frame.height / 2; ++row) {
        memcpy(owned.bytes.data() + size_t(frame.width) * frame.height + size_t(row) * frame.width,
                frame.data + frame.uvOffset + size_t(row) * frame.uvStride, frame.width);
    }
    owned.source = frame.source;
    owned.gyro = frame.gyro;
    frames_.push_back(std::move(owned));
    ++stats_.pendingFrames;
    stats_.pendingBytes += bytes;
    stats_.peakPendingBytes = std::max(stats_.peakPendingBytes, stats_.pendingBytes);
    lastEnqueuedTimestampNs_ = frame.source.timestampNs;
    hasEnqueuedTimestamp_ = true;
    wake_.notify_one();
    return OK;
}

int32_t QCamera3PanoramaWorker::requestJpeg(uint32_t requestFrame, uint32_t quality,
        size_t maxJpegBytes, Completion completion) {
    if (quality < 1 || quality > 100 || maxJpegBytes < 1024 || !completion) {
        return QCamera3OppoPanorama::E_PARAM;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (stats_.state != State::Capturing || cancelRequested_) return E_BUSY;
    requestFrame_ = requestFrame;
    quality_ = quality;
    maxJpegBytes_ = maxJpegBytes;
    completion_ = std::move(completion);
    finishRequested_ = true;
    stats_.state = State::Finalizing;
    wake_.notify_one();
    return OK;
}

void QCamera3PanoramaWorker::cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto &frame : frames_) {
        --stats_.pendingFrames;
        stats_.pendingBytes -= frame.bytes.size();
    }
    frames_.clear();
    stats_.state = State::Canceled;
    cancelRequested_ = true;
    wake_.notify_one();
}

void QCamera3PanoramaWorker::cancelAndDrain() {
    cancel();
    std::unique_lock<std::mutex> lock(mutex_);
    drained_.wait(lock, [this] { return !working_ && !cancelRequested_ && !initRequested_; });
}

QCamera3PanoramaWorker::Stats QCamera3PanoramaWorker::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

std::vector<QCamera3PanoramaWorker::Source> QCamera3PanoramaWorker::retainedSources() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = sources_;
    if (hasWorkingSource_) result.push_back(workingSource_);
    for (const auto &frame : frames_) result.push_back(frame.source);
    return result;
}

QCamera3PanoramaWorker::Result QCamera3PanoramaWorker::finish(QCamera3OppoPanorama &panorama,
        uint32_t requestFrame, uint32_t quality, size_t maxJpegBytes) {
    Result result;
    result.requestFrame = requestFrame;
    result.sources = sources_;
    result.status = panorama.finetune();
    if (result.status != QCamera3OppoPanorama::OK) return result;
    std::vector<uint8_t> nv21(panorama.outputSize());
    QCamera3OppoPanorama::Image output;
    output.data = nv21.data();
    output.length = nv21.size();
    result.status = panorama.stitch(&output);
    if (result.status != QCamera3OppoPanorama::OK) return result;
    QCamera3PanoramaJpeg::Config jpeg;
    jpeg.width = output.width;
    jpeg.height = output.height;
    jpeg.quality = quality;
    // stitch already rotates the pixels; tagging that angle again rotates twice.
    jpeg.orientationDeg = 0;
    jpeg.maxJpegBytes = maxJpegBytes;
    result.status = QCamera3PanoramaJpeg::encode(nv21.data(), output.length, jpeg, result.jpeg) ==
            QCamera3PanoramaJpeg::OK ? OK : E_JPEG;
    if (result.status == OK) {
        result.width = output.width;
        result.height = output.height;
    }
    return result;
}

void QCamera3PanoramaWorker::complete(Result result, Completion completion) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (cancelRequested_) {
            result.status = QCamera3OppoPanorama::E_CANCELED;
            result.jpeg.clear();
            result.width = result.height = 0;
        }
        stats_.lastStatus = result.status;
        stats_.state = result.status == OK ? State::Completed :
                (result.status == QCamera3OppoPanorama::E_CANCELED ? State::Canceled : State::Failed);
    }
    if (completion) completion(std::move(result));
    {
        std::lock_guard<std::mutex> lock(mutex_);
        working_ = false;
        drained_.notify_all();
    }
}

void QCamera3PanoramaWorker::run() {
    QCamera3OppoPanorama panorama;
    for (;;) {
        std::unique_lock<std::mutex> lock(mutex_);
        wake_.wait(lock, [this] {
            return stopping_ || cancelRequested_ || initRequested_ || !frames_.empty() || finishRequested_;
        });
        if (stopping_) return;
        working_ = true;
        if (cancelRequested_) {
            cancelRequested_ = false;
            initRequested_ = false;
            finishRequested_ = false;
            Result result;
            result.requestFrame = requestFrame_;
            result.status = QCamera3OppoPanorama::E_CANCELED;
            result.sources = sources_;
            auto completion = std::move(completion_);
            lock.unlock();
            panorama.cancel();
            panorama.deinit();
            complete(std::move(result), std::move(completion));
            continue;
        }
        if (initRequested_) {
            initRequested_ = false;
            const Config config = config_;
            lock.unlock();
            int32_t status = panorama.init(config.panorama);
            if (status == OK && panorama.outputSize() > config.maxOutputBytes) {
                panorama.deinit();
                status = QCamera3OppoPanorama::E_NOMEM;
            }
            lock.lock();
            if (!cancelRequested_) {
                stats_.state = status == OK ? State::Capturing : State::Failed;
                stats_.lastStatus = status;
            }
            working_ = false;
            drained_.notify_all();
            continue;
        }
        if (!frames_.empty()) {
            OwnedFrame frame = std::move(frames_.front());
            frames_.pop_front();
            workingSource_ = frame.source;
            hasWorkingSource_ = true;
            const uint32_t width = config_.panorama.previewWidth;
            const uint32_t height = config_.panorama.previewHeight;
            lock.unlock();
            QCamera3OppoPanorama::Frame input;
            input.data = frame.bytes.data();
            input.length = frame.bytes.size();
            input.width = width;
            input.height = height;
            input.yStride = input.uvStride = width;
            input.timestampUs = frame.source.timestampNs / 1000;
            const int32_t status = panorama.addFrame(input, &frame.gyro);
            lock.lock();
            --stats_.pendingFrames;
            stats_.pendingBytes -= frame.bytes.size();
            hasWorkingSource_ = false;
            if (status == OK) {
                ++stats_.acceptedFrames;
                sources_.push_back(frame.source);
            } else {
                ++stats_.rejectedFrames;
            }
            stats_.lastStatus = status;
            working_ = false;
            drained_.notify_all();
            continue;
        }
        finishRequested_ = false;
        const uint32_t requestFrame = requestFrame_;
        const uint32_t quality = quality_;
        const size_t maxJpegBytes = maxJpegBytes_;
        auto completion = std::move(completion_);
        lock.unlock();
        Result result = finish(panorama, requestFrame, quality, maxJpegBytes);
        panorama.deinit();
        complete(std::move(result), std::move(completion));
    }
}

} // namespace qcamera
