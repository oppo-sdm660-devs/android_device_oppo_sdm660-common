/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3PreviewBokehWorker.h"
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <limits>

namespace qcamera {

QCamera3PreviewBokehWorker::~QCamera3PreviewBokehWorker() { stop(); }

int32_t QCamera3PreviewBokehWorker::configure(const Backend::Config &config,
        const Backend::Calibration &calibration, int64_t timeoutNs) {
    stop();
    if (timeoutNs <= 0 || timeoutNs > 1000000000LL) return -EINVAL;
    const int32_t rc = mBackend.configure(config, calibration);
    if (rc != 0) return rc;
    mConfig = config;
    mCalibration = calibration;
    mTimeoutNs = timeoutNs;
    mCancelled = false;
    mRunning = true;
    mResetRequired = false;
    mThread = std::thread(&QCamera3PreviewBokehWorker::run, this);
    return 0;
}

void QCamera3PreviewBokehWorker::metadata(const Source &source) {
    if (source.timestampNs <= 0) return;
    std::lock_guard<std::mutex> lock(mMutex);
    if (!mRunning) return;
    for (unsigned i = 0; i < 2; ++i) if (source.cameraHandle == mConfig.cameraHandles[i]) {
        auto &records = mMetadata[i];
        records[source.frameNumber] = source.timestampNs;
        while (records.size() > 16) records.erase(records.begin());
        mCondition.notify_all();
        break;
    }
}

bool QCamera3PreviewBokehWorker::copy(const Image &source, bool nv12, Frame &frame) {
    if (!QCamera3HdrAlgo::validImage(source)) return false;
    const size_t ySize = size_t(source.width) * source.height;
    frame.pixels.resize(ySize * 3 / 2);
    for (uint32_t y = 0; y < source.height; ++y)
        memcpy(frame.pixels.data() + size_t(y) * source.width,
                source.data + source.yOffset + size_t(y) * source.yStride, source.width);
    for (uint32_t y = 0; y < source.height / 2; ++y) {
        uint8_t *destination = frame.pixels.data() + ySize + size_t(y) * source.width;
        const uint8_t *row = source.data + source.uvOffset + size_t(y) * source.uvStride;
        for (uint32_t x = 0; x < source.width; x += 2) {
            destination[x] = row[x + (nv12 ? 1 : 0)];
            destination[x + 1] = row[x + (nv12 ? 0 : 1)];
        }
    }
    return true;
}

QCamera3PreviewBokehWorker::Image QCamera3PreviewBokehWorker::image(Frame &frame,
        uint32_t width, uint32_t height) {
    return {frame.pixels.data(), frame.pixels.size(), width, height, 0,
        size_t(width) * height, width, width, height, height / 2};
}

int32_t QCamera3PreviewBokehWorker::auxiliary(const Source &source,
        const Image &input, bool nv12) {
    if (source.cameraHandle != mConfig.cameraHandles[1] || source.timestampNs <= 0 ||
            input.width != mConfig.imageWidths[1] || input.height != mConfig.imageHeights[1]) return -EINVAL;
    Frame frame;
    frame.source = source;
    if (!copy(input, nv12, frame)) return -EINVAL;
    std::lock_guard<std::mutex> lock(mMutex);
    if (!mRunning || mCancelled) return -ECANCELED;
    mAuxiliary.push_back(std::move(frame));
    while (mAuxiliary.size() > 4) mAuxiliary.pop_front();
    mCondition.notify_all();
    return 0;
}

bool QCamera3PreviewBokehWorker::pairLocked(Job &job, Frame &auxiliary) {
    const auto &source = job.main.source;
    const auto mainMeta = mMetadata[0].find(source.frameNumber);
    // V4L2's timeval image timestamp loses sub-microsecond sensor precision.
    if (mainMeta == mMetadata[0].end() || std::abs(mainMeta->second - source.timestampNs) > 1000) return false;
    auto best = mAuxiliary.end();
    int64_t nearest = std::numeric_limits<int64_t>::max();
    for (auto it = mAuxiliary.begin(); it != mAuxiliary.end(); ++it) {
        const auto meta = mMetadata[1].find(it->source.frameNumber);
        if (meta == mMetadata[1].end() || std::abs(meta->second - it->source.timestampNs) > 1000) continue;
        const int64_t delta = std::abs(mainMeta->second - meta->second);
        if (delta <= mConfig.maxTimestampDeltaNs && delta < nearest) { best = it; nearest = delta; }
    }
    if (best == mAuxiliary.end()) return false;
    auxiliary = std::move(*best);
    mAuxiliary.erase(best);
    job.main.source.timestampNs = mainMeta->second;
    auxiliary.source.timestampNs = mMetadata[1].at(auxiliary.source.frameNumber);
    return true;
}

int32_t QCamera3PreviewBokehWorker::process(const Source &source, const Image &input,
        bool nv12, const Image &destination, const Backend::Focus &focus, Backend::Result &result) {
    result = {};
    if (source.cameraHandle != mConfig.cameraHandles[0] || source.timestampNs <= 0 ||
            input.width != mConfig.imageWidths[0] || input.height != mConfig.imageHeights[0] ||
            !QCamera3HdrAlgo::validImage(destination) || destination.width != input.width ||
            destination.height != input.height) return -EINVAL;
    auto job = std::make_shared<Job>();
    job->main.source = source;
    job->focus = focus;
    if (!copy(input, nv12, job->main)) return -EINVAL;
    std::unique_lock<std::mutex> lock(mMutex);
    if (!mRunning || mCancelled) return -ECANCELED;
    if (mJob) return -EBUSY;
    mJob = job;
    mCondition.notify_all();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds(mTimeoutNs);
    if (!mCondition.wait_until(lock, deadline, [&] { return job->done || job->cancelled.load() || mCancelled.load(); })) {
        job->cancelled = true;
        if (mJob == job) mJob.reset();
        mCondition.notify_all();
        return -ETIMEDOUT;
    }
    if (mCancelled || job->cancelled) return -ECANCELED;
    result = job->result;
    if (job->status != 0) return job->status;
    const size_t ySize = size_t(input.width) * input.height;
    for (uint32_t y = 0; y < input.height; ++y)
        memcpy(destination.data + destination.yOffset + size_t(y) * destination.yStride,
                job->output.data() + size_t(y) * input.width, input.width);
    for (uint32_t y = 0; y < input.height / 2; ++y) {
        uint8_t *row = destination.data + destination.uvOffset + size_t(y) * destination.uvStride;
        const uint8_t *output = job->output.data() + ySize + size_t(y) * input.width;
        for (uint32_t x = 0; x < input.width; x += 2) {
            row[x] = output[x + (nv12 ? 1 : 0)];
            row[x + 1] = output[x + (nv12 ? 0 : 1)];
        }
    }
    return 0;
}

void QCamera3PreviewBokehWorker::run() {
    std::unique_lock<std::mutex> lock(mMutex);
    while (!mCancelled) {
        Frame auxiliary;
        mCondition.wait(lock, [&] { return mCancelled.load() || (mJob && pairLocked(*mJob, auxiliary)); });
        if (mCancelled) break;
        auto job = mJob;
        mJob.reset();
        mActiveJob = job;
        const bool reset = mResetRequired;
        mResetRequired = false;
        lock.unlock();
        const Image wide = image(job->main, mConfig.imageWidths[0], mConfig.imageHeights[0]);
        const Image tele = image(auxiliary, mConfig.imageWidths[1], mConfig.imageHeights[1]);
        job->output.resize(job->main.pixels.size());
        const Image output = {job->output.data(), job->output.size(), wide.width, wide.height,
            0, size_t(wide.width) * wide.height, wide.width, wide.width, wide.height, wide.height / 2};
        const Backend::Pair pair = {{job->main.source.cameraHandle, auxiliary.source.cameraHandle},
            {job->main.source.frameNumber, auxiliary.source.frameNumber},
            {job->main.source.timestampNs, auxiliary.source.timestampNs}};
        int32_t status = reset ? mBackend.configure(mConfig, mCalibration) : 0;
        if (status == 0) status = mBackend.process(pair, wide, tele, output,
                job->focus, job->result, &job->cancelled);
        lock.lock();
        job->status = status;
        job->done = true;
        mActiveJob.reset();
        mCondition.notify_all();
    }
}

void QCamera3PreviewBokehWorker::cancel() {
    std::lock_guard<std::mutex> lock(mMutex);
    mCancelled = true;
    if (mJob) mJob->cancelled = true;
    if (mActiveJob) mActiveJob->cancelled = true;
    mCondition.notify_all();
}

void QCamera3PreviewBokehWorker::flush() {
    std::lock_guard<std::mutex> lock(mMutex);
    if (mJob) mJob->cancelled = true;
    if (mActiveJob) mActiveJob->cancelled = true;
    mJob.reset();
    mAuxiliary.clear();
    mMetadata[0].clear(); mMetadata[1].clear();
    mCondition.notify_all();
}

void QCamera3PreviewBokehWorker::resetTemporalState() {
    std::lock_guard<std::mutex> lock(mMutex);
    mResetRequired = true;
}

void QCamera3PreviewBokehWorker::stop() {
    cancel();
    if (mThread.joinable()) mThread.join();
    std::lock_guard<std::mutex> lock(mMutex);
    mRunning = false;
    mJob.reset();
    mAuxiliary.clear();
    mMetadata[0].clear(); mMetadata[1].clear();
    mBackend.reset();
}
} // namespace qcamera
