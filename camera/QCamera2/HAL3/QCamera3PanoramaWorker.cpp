/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3PanoramaWorker.h"
#include "QCamera3PanoramaJpeg.h"
#include "QCamera3PanoramaGyro.h"
#include "QCamera3MmsGyroSource.h"

#include <algorithm>
#include <cstring>
#include <cmath>
#include <utility>
#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace qcamera {
namespace {
bool planeFits(size_t offset, uint32_t stride, uint32_t width, uint32_t rows, size_t length) {
    if (offset > length || stride < width || rows == 0) return false;
    const size_t available = length - offset;
    return width <= available && size_t(rows - 1) <= (available - width) / stride;
}
int32_t frontStatus(QCamera3OppoVisidonPanorama::Status status) {
    using Front = QCamera3OppoVisidonPanorama;
    switch (status) {
        case Front::OK: return QCamera3PanoramaWorker::OK;
        case Front::E_STATE: return QCamera3OppoPanorama::E_STATE;
        case Front::E_PARAM: return QCamera3OppoPanorama::E_PARAM;
        case Front::E_NOMEM: return QCamera3OppoPanorama::E_NOMEM;
        case Front::E_LIMIT: return QCamera3OppoPanorama::E_LIMIT;
        case Front::E_FRAMES: return QCamera3OppoPanorama::E_NOFRAME;
        case Front::E_CANCELED: return QCamera3OppoPanorama::E_CANCELED;
        case Front::E_LOAD: return QCamera3OppoPanorama::E_UNSUPPORTED;
        case Front::E_ENCODE:
        case Front::E_JPEG: return QCamera3PanoramaWorker::E_JPEG;
        default: return QCamera3PanoramaWorker::E_BACKEND;
    }
}

int32_t failureReason(int32_t status) {
    switch (status) {
        case QCamera3PanoramaWorker::OK: return 0;
        case QCamera3OppoPanorama::E_UNSUPPORTED: return 1;
        case QCamera3OppoPanorama::E_NOFRAME: return 3;
        case QCamera3OppoPanorama::E_PARAM: return 7;
        case QCamera3PanoramaWorker::E_JPEG: return 9;
        case QCamera3OppoPanorama::E_NOMEM: return 10;
        case QCamera3OppoPanorama::E_CANCELED: return 12;
        default: return 13;
    }
}

uint32_t frontDimension(uint32_t dimension, double scale) {
    return std::max(8u, uint32_t(std::lround(dimension * scale / 8.0)) * 8u);
}

// Pixel centres are mapped in each plane; interleaved V/U remain separate channels.
void resizePlane(const uint8_t *input, uint32_t width, uint32_t height,
        uint8_t *output, uint32_t outWidth, uint32_t outHeight, uint32_t channels) {
    struct Sample { uint32_t low, high, weight; };
    auto sample = [](uint32_t index, uint32_t source, uint32_t destination) {
        const double coordinate = std::max(0.0, std::min(double(source - 1),
                (index + 0.5) * source / destination - 0.5));
        const uint32_t low = uint32_t(coordinate);
        return Sample{low, std::min(low + 1, source - 1),
                uint32_t(std::lround((coordinate - low) * 256.0))};
    };
    std::vector<Sample> columns(outWidth);
    for (uint32_t x = 0; x < outWidth; ++x) columns[x] = sample(x, width, outWidth);
    for (uint32_t y = 0; y < outHeight; ++y) {
        const auto row = sample(y, height, outHeight);
        for (uint32_t x = 0; x < outWidth; ++x) {
            const auto column = columns[x];
            for (uint32_t c = 0; c < channels; ++c) {
                const uint8_t *top = input + size_t(row.low) * width * channels;
                const uint8_t *bottom = input + size_t(row.high) * width * channels;
                const uint32_t a = top[column.low * channels + c] * (256 - column.weight) +
                        top[column.high * channels + c] * column.weight;
                const uint32_t b = bottom[column.low * channels + c] * (256 - column.weight) +
                        bottom[column.high * channels + c] * column.weight;
                output[(size_t(y) * outWidth + x) * channels + c] =
                        uint8_t((a * (256 - row.weight) + b * row.weight + 32768) / 65536);
            }
        }
    }
}

std::vector<uint8_t> resizeNv21(const std::vector<uint8_t> &input,
        uint32_t width, uint32_t height, uint32_t outWidth, uint32_t outHeight) {
    const size_t outY = size_t(outWidth) * outHeight;
    std::vector<uint8_t> output(outY * 3 / 2);
    resizePlane(input.data(), width, height, output.data(), outWidth, outHeight, 1);
    resizePlane(input.data() + size_t(width) * height, width / 2, height / 2,
            output.data() + outY, outWidth / 2, outHeight / 2, 2);
    return output;
}

void rotateNv21(std::vector<uint8_t> &bytes, uint32_t &width, uint32_t &height, int32_t angle) {
    if (angle == 0) return;
    const uint32_t outWidth = angle == 180 ? width : height;
    const uint32_t outHeight = angle == 180 ? height : width;
    std::vector<uint8_t> rotated(bytes.size());
    auto target = [angle](uint32_t x, uint32_t y, uint32_t w, uint32_t h,
            uint32_t *outX, uint32_t *outY) {
        *outX = angle == 90 ? h - 1 - y : angle == 180 ? w - 1 - x : y;
        *outY = angle == 90 ? x : angle == 180 ? h - 1 - y : w - 1 - x;
    };
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            uint32_t dx, dy;
            target(x, y, width, height, &dx, &dy);
            rotated[size_t(dy) * outWidth + dx] = bytes[size_t(y) * width + x];
        }
    }
    const size_t uv = size_t(width) * height;
    for (uint32_t y = 0; y < height / 2; ++y) {
        for (uint32_t x = 0; x < width / 2; ++x) {
            uint32_t dx, dy;
            target(x, y, width / 2, height / 2, &dx, &dy);
            memcpy(rotated.data() + uv + size_t(dy) * outWidth + 2 * dx,
                    bytes.data() + uv + size_t(y) * width + 2 * x, 2);
        }
    }
    bytes.swap(rotated);
    width = outWidth;
    height = outHeight;
}

}

struct QCamera3PanoramaWorker::GyroFeed : QCamera3MmsGyroSource::Listener {
    QCamera3MmsGyroSource source;
    QCamera3PanoramaGyro timeline;
    int32_t start(int32_t mountAngle) {
        timeline.reset(mountAngle);
        return source.start(*this, 5000000);
    }
    void stop() { source.stop(); }
    int32_t onGyro(int64_t timestampNs, const float angularVelocity[3]) override {
        return timeline.add(timestampNs, angularVelocity);
    }
};

QCamera3PanoramaWorker::QCamera3PanoramaWorker() {
    gyroFeed_.reset(new GyroFeed);
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
            config.maxOutputBytes == 0 || config.minFrameIntervalNs < 0 ||
            (config.useGyro && config.sensorMountAngle != 0 && config.sensorMountAngle != 90 &&
             config.sensorMountAngle != 180 && config.sensorMountAngle != 270) ||
            (config.backend == Backend::FrontVisidon && (config.focalPx <= 0 ||
                (config.panorama.orientationDeg != 0 && config.panorama.orientationDeg != 90 &&
                 config.panorama.orientationDeg != 180 && config.panorama.orientationDeg != 270)))) {
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
    if (stats_.state == State::Failed) return stats_.lastStatus;
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
    if ((stats_.state != State::Capturing && stats_.state != State::Failed) ||
            cancelRequested_ || finishRequested_) return E_BUSY;
    requestFrame_ = requestFrame;
    quality_ = quality;
    maxJpegBytes_ = maxJpegBytes;
    completion_ = std::move(completion);
    finishRequested_ = true;
    if (stats_.state != State::Failed) stats_.state = State::Finalizing;
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
    result.failureReason = failureReason(result.status);
    if (result.status != QCamera3OppoPanorama::OK) return result;
    if (panorama.outputSize() > config_.maxOutputBytes) {
        result.status = QCamera3OppoPanorama::E_NOMEM;
        result.failureReason = failureReason(result.status);
        return result;
    }
    std::vector<uint8_t> nv21(panorama.outputSize());
    QCamera3OppoPanorama::Image output;
    output.data = nv21.data();
    output.length = nv21.size();
    result.status = panorama.stitch(&output);
    result.failureReason = failureReason(result.status);
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
    result.failureReason = failureReason(result.status);
    if (result.status == OK) {
        result.width = output.width;
        result.height = output.height;
    }
    return result;
}

QCamera3PanoramaWorker::Result QCamera3PanoramaWorker::finish(
        QCamera3OppoVisidonPanorama &panorama, uint32_t requestFrame,
        uint32_t quality, size_t maxJpegBytes) {
    Result result;
    result.requestFrame = requestFrame;
    result.sources = sources_;
    std::vector<uint8_t> nv21(panorama.config().maxOutputBytes);
    QCamera3OppoVisidonPanorama::Image image{nv21.data(), nv21.size(), 0, 0, 0};
    const auto status = panorama.captureNv21(&image);
    result.status = frontStatus(status);
    result.failureReason = QCamera3OppoVisidonPanorama::halFailureReason(status);
    if (result.status != OK) return result;
    nv21.resize(image.length);
    rotateNv21(nv21, image.width, image.height, config_.panorama.orientationDeg);
    QCamera3PanoramaJpeg::Config jpeg;
    jpeg.width = image.width;
    jpeg.height = image.height;
    jpeg.quality = quality;
    jpeg.orientationDeg = 0;
    jpeg.maxJpegBytes = maxJpegBytes;
    result.status = QCamera3PanoramaJpeg::encode(nv21.data(), nv21.size(), jpeg, result.jpeg) ==
            QCamera3PanoramaJpeg::OK ? OK : E_JPEG;
    result.failureReason = failureReason(result.status);
    if (result.status == OK) {
        result.width = image.width;
        result.height = image.height;
    }
    return result;
}

void QCamera3PanoramaWorker::complete(Result result, Completion completion) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (cancelRequested_) {
            result.status = QCamera3OppoPanorama::E_CANCELED;
            result.failureReason = 12;
            result.jpeg.clear();
            result.width = result.height = 0;
        }
        stats_.lastStatus = result.status;
        stats_.failureReason = result.failureReason;
        stats_.state = result.status == OK ? State::Completed :
                (result.status == QCamera3OppoPanorama::E_CANCELED ? State::Canceled : State::Failed);
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_INFO, "OCamPanorama",
                "complete frame=%u status=%d reason=%d size=%ux%u sources=%zu gyroStart=%d gyroStatus=%d matched=%u missing=%u",
                result.requestFrame, result.status, result.failureReason, result.width, result.height,
                result.sources.size(), stats_.gyroFeedStarted, stats_.gyroFeedStatus,
                stats_.gyroMatchedFrames, stats_.gyroMissingFrames);
#endif
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
    QCamera3OppoVisidonPanorama front;
    QCamera3OppoVisidonPanorama::Position position;
    int32_t minKeyframePx = 0, maxKeyframePx = 0;
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
            result.failureReason = 12;
            result.sources = sources_;
            auto completion = std::move(completion_);
            lock.unlock();
            gyroFeed_->stop();
            panorama.cancel();
            panorama.deinit();
            front.cancel();
            front.deinit();
            complete(std::move(result), std::move(completion));
            continue;
        }
        if (initRequested_) {
            initRequested_ = false;
            const Config config = config_;
            lock.unlock();
            gyroFeed_->stop();
            position = QCamera3OppoVisidonPanorama::Position();
            minKeyframePx = maxKeyframePx = 0;
            int32_t status;
            if (config.backend == Backend::FrontVisidon) {
                QCamera3OppoVisidonPanorama::Config frontConfig;
                const double scale = std::min(1.0, 1440.0 /
                        std::max(config.panorama.previewWidth, config.panorama.previewHeight));
                // Visidon quarters NV21 internally: both quarter dimensions must stay even.
                frontConfig.previewWidth = frontDimension(config.panorama.previewWidth, scale);
                frontConfig.previewHeight = frontDimension(config.panorama.previewHeight, scale);
                frontConfig.focalPx = std::max(1, int32_t(std::lround(double(config.focalPx) *
                        frontConfig.previewWidth / config.panorama.previewWidth)));
                // Stock's input axis is independent of the final JPEG rotation.
                frontConfig.transposed = true;
                frontConfig.rotationDeg = 0;
                frontConfig.maxOutputBytes = std::min(config.maxOutputBytes, size_t(16u * 1024u * 1024u));
                frontConfig.testEngine = config.testVisidonEngine;
                status = frontStatus(front.init(frontConfig));
            } else status = panorama.init(config.panorama);
            if (config.backend == Backend::RearOss && status == OK && panorama.outputSize() > config.maxOutputBytes) {
                panorama.deinit();
                status = QCamera3OppoPanorama::E_NOMEM;
            }
            const bool wantsGyro = status == OK && config.backend == Backend::RearOss && config.useGyro;
            const int32_t gyroStatus = wantsGyro ? (config.panorama.pxPerDeg > 0.0f ?
                    gyroFeed_->start(config.sensorMountAngle) : -EINVAL) : 0;
            lock.lock();
            stats_.gyroFeedStatus = gyroStatus;
            stats_.gyroFeedStarted = wantsGyro && gyroStatus == 0;
#ifdef __ANDROID__
            __android_log_print(ANDROID_LOG_INFO, "OCamPanorama",
                    "init backend=%d input=%ux%u rotation=%d sweepY=%d mount=%d pxPerDeg=%g status=%d gyroStatus=%d",
                    int(config.backend), config.panorama.previewWidth, config.panorama.previewHeight,
                    config.panorama.orientationDeg, config.panorama.verticalSweep, config.sensorMountAngle,
                    double(config.panorama.pxPerDeg), status, gyroStatus);
#endif
            if (!cancelRequested_) {
                stats_.state = status == OK ? State::Capturing : State::Failed;
                stats_.lastStatus = status;
                stats_.failureReason = failureReason(status);
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
            const bool isFront = config_.backend == Backend::FrontVisidon;
            const bool firstKeyframe = sources_.empty();
            const int64_t previousTimestampNs = firstKeyframe ? 0 : sources_.back().timestampNs;
            const bool useGyro = config_.useGyro && !isFront;
            lock.unlock();
            QCamera3OppoPanorama::Frame input;
            input.data = frame.bytes.data();
            input.length = frame.bytes.size();
            input.width = width;
            input.height = height;
            input.yStride = input.uvStride = width;
            input.timestampUs = frame.source.timestampNs / 1000;
            int32_t status;
            bool skipped = false;
            if (isFront) {
                const auto &frontConfig = front.config();
                std::vector<uint8_t> normalized;
                if (width != frontConfig.previewWidth || height != frontConfig.previewHeight) {
                    normalized = resizeNv21(frame.bytes, width, height,
                            frontConfig.previewWidth, frontConfig.previewHeight);
                }
                QCamera3OppoVisidonPanorama::Frame frontInput;
                frontInput.y = normalized.empty() ? input.data : normalized.data();
                frontInput.width = frontConfig.previewWidth;
                frontInput.height = frontConfig.previewHeight;
                frontInput.uv = frontInput.y + size_t(frontInput.width) * frontInput.height;
                frontInput.yStride = frontInput.uvStride = frontInput.width;
                frontInput.timestampUs = input.timestampUs;
                auto measured = front.measureOverlap(frontInput, &position);
                status = frontStatus(measured);
                if (measured == QCamera3OppoVisidonPanorama::OK) {
                    const int32_t displacement = int32_t(std::lround(position.y * frontInput.height));
                    // Centre, then spaced views; do not stitch consecutive stationary frames.
                    const int32_t stepPx = std::max(2, int32_t(std::lround(
                            2.0 * frontConfig.focalPx * std::tan(5.0 * M_PI / 360.0))));
                    skipped = !firstKeyframe && (position.quality < 2 ||
                            (displacement > minKeyframePx - stepPx &&
                             displacement < maxKeyframePx + stepPx));
                    if (!skipped) {
                        frontInput.displacementPx = firstKeyframe ? 0 : displacement;
                        status = frontStatus(front.addFrame(frontInput));
                        if (status == OK) {
                            minKeyframePx = std::min(minKeyframePx, frontInput.displacementPx);
                            maxKeyframePx = std::max(maxKeyframePx, frontInput.displacementPx);
                        }
                    }
                }
            } else {
                if (useGyro) frame.gyro = gyroFeed_->timeline.interval(previousTimestampNs,
                        frame.source.timestampNs);
                status = panorama.addFrame(input, &frame.gyro);
            }
            const bool terminalFailure = status != OK && status != QCamera3OppoPanorama::E_LIMIT &&
                    status != QCamera3OppoPanorama::E_OVERFLOW;
            if (terminalFailure) gyroFeed_->stop();
            lock.lock();
            --stats_.pendingFrames;
            stats_.pendingBytes -= frame.bytes.size();
            hasWorkingSource_ = false;
            if (skipped) {
                ++stats_.droppedFrames;
            } else if (status == OK) {
                ++stats_.acceptedFrames;
                sources_.push_back(frame.source);
                if (useGyro && !firstKeyframe) {
                    if (frame.gyro.valid) ++stats_.gyroMatchedFrames;
                    else ++stats_.gyroMissingFrames;
                }
            } else {
                ++stats_.rejectedFrames;
            }
            stats_.lastStatus = status;
            if (terminalFailure && !cancelRequested_) {
                stats_.state = State::Failed;
                stats_.failureReason = failureReason(status);
                for (const auto &queued : frames_) {
                    --stats_.pendingFrames;
                    stats_.pendingBytes -= queued.bytes.size();
                    ++stats_.rejectedFrames;
                }
                frames_.clear();
            }
            working_ = false;
            drained_.notify_all();
            continue;
        }
        finishRequested_ = false;
        const uint32_t requestFrame = requestFrame_;
        const uint32_t quality = quality_;
        const size_t maxJpegBytes = maxJpegBytes_;
        const int32_t terminalStatus = stats_.state == State::Failed ? stats_.lastStatus : OK;
        auto completion = std::move(completion_);
        lock.unlock();
        gyroFeed_->stop();
        Result result;
        if (terminalStatus != OK) {
            result.requestFrame = requestFrame;
            result.status = terminalStatus;
            result.failureReason = failureReason(terminalStatus);
            result.sources = sources_;
        } else result = config_.backend == Backend::FrontVisidon ?
                    finish(front, requestFrame, quality, maxJpegBytes) :
                    finish(panorama, requestFrame, quality, maxJpegBytes);
        panorama.deinit();
        front.deinit();
        complete(std::move(result), std::move(completion));
    }
}

} // namespace qcamera
