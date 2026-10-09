/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3PreviewBokehWorker.h"
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <pthread.h>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif
#if defined(__ANDROID__)
#include "QCameraTrace.h"
#else
#define KPI_ATRACE_NAME(name) ((void)0)
#endif

namespace qcamera {

QCamera3PreviewBokehWorker::~QCamera3PreviewBokehWorker() { stop(); }

int32_t QCamera3PreviewBokehWorker::configure(const Backend::Config &config,
        const Backend::Calibration &calibration, int64_t timeoutNs, const uint32_t *sessionIds) {
    stop();
    if (timeoutNs <= 0 || timeoutNs > 1000000000LL) return -EINVAL;
    const int32_t rc = mBackend.configure(config, calibration);
    if (rc != 0) return rc;
    mConfig = config;
    mCalibration = calibration;
    for (unsigned i = 0; i < 2; ++i) mSessionIds[i] = sessionIds ? sessionIds[i] : 0;
    mTimeoutNs = timeoutNs;
    mCancelled = false;
    mRunning = true;
    mResetRequired = false;
    mThread = std::thread(&QCamera3PreviewBokehWorker::run, this);
    return 0;
}

void QCamera3PreviewBokehWorker::faces(const QCamera3PreviewBokehFaces::Observation &source) {
    std::lock_guard<std::mutex> lock(mMutex);
    if (!mRunning || mCancelled || !mSessionIds[0] ||
            source.cameraHandle != mConfig.cameraHandles[0] || source.sessionId != mSessionIds[0] ||
            source.count > QCamera3PreviewBokehFaces::kMaxFaces) return;
    mFaces.emplace(source.nativeFrameNumber, source);
    // Match the existing 16-native metadata window, without delaying an image for FD.
    while (mFaces.size() > 16) mFaces.erase(mFaces.begin());
}

void QCamera3PreviewBokehWorker::selectFacesLocked(Job &job) {
    const uint32_t native = job.main.source.nativeFrameNumber;
    const auto exact = mFaces.find(native);
    if (exact != mFaces.end()) {
        job.faceFrame = exact->first;
        const auto &source = exact->second;
        if (mFaceSelector.select(source, job.selectedFace)) {
            job.faceCount = source.count;
            job.faceStatus = 1;
        } else job.faceStatus = source.count ? 0 : 2;
        mFaces.erase(exact);
    } else {
        const auto after = mFaces.upper_bound(native);
        if (after != mFaces.begin()) {
            job.faceStatus = 3;
            job.faceFrame = std::prev(after)->first;
        }
    }
    mFaces.erase(mFaces.begin(), mFaces.lower_bound(native));
}

void QCamera3PreviewBokehWorker::metadata(const Metadata &source) {
    if (source.status == 0 && source.exposureStartNs <= 0) return;
    std::lock_guard<std::mutex> lock(mMutex);
    if (!mRunning) return;
    for (unsigned i = 0; i < 2; ++i) if (source.cameraHandle == mConfig.cameraHandles[i]) {
        auto &records = mMetadata[i];
        records[source.nativeFrameNumber] = source;
        while (records.size() > 16) records.erase(records.begin());
        mCondition.notify_all();
        break;
    }
}

bool QCamera3PreviewBokehWorker::copy(const Image &source, bool nv12, Frame &frame) {
    KPI_ATRACE_NAME("ocam.bokeh.worker.copyIn");
    if (!QCamera3HdrAlgo::validImage(source)) return false;
    const size_t ySize = size_t(source.width) * source.height;
    {
        KPI_ATRACE_NAME("ocam.bokeh.worker.allocateIn");
        frame.pixels.resize(ySize * 3 / 2);
    }
    for (uint32_t y = 0; y < source.height; ++y)
        memcpy(frame.pixels.data() + size_t(y) * source.width,
                source.data + source.yOffset + size_t(y) * source.yStride, source.width);
    for (uint32_t y = 0; y < source.height / 2; ++y) {
        uint8_t *destination = frame.pixels.data() + ySize + size_t(y) * source.width;
        const uint8_t *row = source.data + source.uvOffset + size_t(y) * source.uvStride;
        if (!nv12) memcpy(destination, row, source.width);
        else {
            uint32_t x = 0;
#if defined(__ARM_NEON)
            for (; x + 16 <= source.width; x += 16)
                vst1q_u8(destination + x, vrev16q_u8(vld1q_u8(row + x)));
#endif
            for (; x < source.width; x += 2) {
                destination[x] = row[x + 1];
                destination[x + 1] = row[x];
            }
        }
    }
    return true;
}

QCamera3PreviewBokehWorker::Image QCamera3PreviewBokehWorker::image(Frame &frame,
        uint32_t width, uint32_t height) {
    return {frame.pixels.data(), frame.pixels.size(), width, height, 0,
        size_t(width) * height, width, width, height, height / 2};
}

void QCamera3PreviewBokehWorker::recyclePixelsLocked(std::vector<uint8_t> &pixels,
        std::vector<uint8_t> &spare) {
    if (spare.empty()) spare.swap(pixels);
    else std::vector<uint8_t>().swap(pixels);
}

void QCamera3PreviewBokehWorker::recycleJobLocked(Job &job) {
    if (!mRunning || mCancelled) {
        std::vector<uint8_t>().swap(job.main.pixels);
        std::vector<uint8_t>().swap(job.output);
        return;
    }
    recyclePixelsLocked(job.main.pixels, mMainPixels);
    recyclePixelsLocked(job.output, mOutputPixels);
}

int32_t QCamera3PreviewBokehWorker::auxiliary(const Source &source,
        const Image &input, bool nv12) {
    if (source.cameraHandle != mConfig.cameraHandles[1] || source.imageSofNs <= 0 ||
            input.width != mConfig.imageWidths[1] || input.height != mConfig.imageHeights[1]) return -EINVAL;
    Frame frame;
    frame.source = source;
    if (source.geometryStatus == 0) {
        {
            std::lock_guard<std::mutex> lock(mMutex);
            if (!mRunning || mCancelled) return -ECANCELED;
            frame.pixels.swap(mAuxiliaryPixels);
        }
        if (!copy(input, nv12, frame)) {
            std::lock_guard<std::mutex> lock(mMutex);
            recyclePixelsLocked(frame.pixels, mAuxiliaryPixels);
            return -EINVAL;
        }
    }
    std::lock_guard<std::mutex> lock(mMutex);
    if (!mRunning || mCancelled) return -ECANCELED;
    mAuxiliary.push_back(std::move(frame));
    while (mAuxiliary.size() > 4) {
        recyclePixelsLocked(mAuxiliary.front().pixels, mAuxiliaryPixels);
        mAuxiliary.pop_front();
    }
    mCondition.notify_all();
    return 0;
}

int32_t QCamera3PreviewBokehWorker::pairLocked(Job &job, Frame &auxiliary) {
    const auto &source = job.main.source;
    if (source.geometryStatus != 0) return source.geometryStatus;
    const auto mainMeta = mMetadata[0].find(source.nativeFrameNumber);
    if (mainMeta == mMetadata[0].end()) return -EAGAIN;
    job.main.metadata = mainMeta->second;
    if (mainMeta->second.requestFrameNumber != job.requestFrameNumber) return -ESTALE;
    if (mainMeta->second.status != 0) return mainMeta->second.status;
    auto best = mAuxiliary.end();
    int64_t nearest = std::numeric_limits<int64_t>::max();
    bool awaitingMetadata = false, passedWindow = false;
    int32_t failure = 0;
    for (auto it = mAuxiliary.begin(); it != mAuxiliary.end(); ++it) {
        const auto record = mMetadata[1].find(it->source.nativeFrameNumber);
        if (record == mMetadata[1].end()) { awaitingMetadata = true; continue; }
        const auto &meta = record->second;
        if (meta.status != 0) {
            if (meta.requestFrameNumber == job.requestFrameNumber) failure = meta.status;
            continue;
        }
        const int64_t delta = std::abs(mainMeta->second.exposureStartNs - meta.exposureStartNs);
        if (meta.exposureStartNs > mainMeta->second.exposureStartNs &&
                delta > mConfig.maxTimestampDeltaNs) passedWindow = true;
        if (delta > mConfig.maxTimestampDeltaNs) continue;
        if (meta.requestFrameNumber != job.requestFrameNumber) {
            if (!failure) failure = -ESTALE;
            continue;
        }
        if (delta < nearest) { best = it; nearest = delta; }
    }
    if (best == mAuxiliary.end()) {
        // AXI_DONE is a buffer completion time, not an optical timestamp.
        // A queued image without metadata can still fill the exposure window.
        if (awaitingMetadata) return -EAGAIN;
        if (failure) return failure;
        return passedWindow ? -ERANGE : -EAGAIN;
    }
    if (best->source.geometryStatus != 0) return best->source.geometryStatus;
    auxiliary = std::move(*best);
    mAuxiliary.erase(best);
    auxiliary.metadata = mMetadata[1].at(auxiliary.source.nativeFrameNumber);
    return 0;
}

int32_t QCamera3PreviewBokehWorker::process(uint32_t requestFrameNumber, const Source &source,
        const Image &input, bool nv12, const Image &destination, const Backend::Focus &focus,
        Backend::Result &result, Origin *origin) {
    result = {};
    if (origin) *origin = {};
    if (source.cameraHandle != mConfig.cameraHandles[0] || source.imageSofNs <= 0 ||
            input.width != mConfig.imageWidths[0] || input.height != mConfig.imageHeights[0] ||
            !QCamera3HdrAlgo::validImage(destination) || destination.width != input.width ||
            destination.height != input.height) return -EINVAL;
    if (source.geometryStatus != 0) return source.geometryStatus;
    auto job = std::make_shared<Job>();
    job->main.source = source;
    job->requestFrameNumber = requestFrameNumber;
    job->focus = focus;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (!mRunning || mCancelled) return -ECANCELED;
        job->main.pixels.swap(mMainPixels);
    }
    if (!copy(input, nv12, job->main)) {
        std::lock_guard<std::mutex> lock(mMutex);
        recycleJobLocked(*job);
        return -EINVAL;
    }
    std::unique_lock<std::mutex> lock(mMutex);
    if (!mRunning || mCancelled || mJob) {
        recycleJobLocked(*job);
        return !mRunning || mCancelled ? -ECANCELED : -EBUSY;
    }
    mJob = job;
    mCondition.notify_all();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds(mTimeoutNs);
    bool completed;
    {
        KPI_ATRACE_NAME("ocam.bokeh.worker.waitForJob");
        completed = mCondition.wait_until(lock, deadline, [&] {
            return job->done || job->cancelled.load() || mCancelled.load();
        });
    }
    if (!completed) {
        job->cancelled = true;
        if (mJob == job) {
            mJob.reset();
            recycleJobLocked(*job);
        }
        mCondition.notify_all();
        return -ETIMEDOUT;
    }
    if (mCancelled || job->cancelled) {
        if (mActiveJob != job) recycleJobLocked(*job);
        return -ECANCELED;
    }
    result = job->result;
    if (origin) *origin = { { job->main.metadata, job->auxiliaryMetadata } };
    if (job->status != 0) {
        recycleJobLocked(*job);
        return job->status;
    }
    KPI_ATRACE_NAME("ocam.bokeh.worker.copyOut");
    const size_t ySize = size_t(input.width) * input.height;
    for (uint32_t y = 0; y < input.height; ++y)
        memcpy(destination.data + destination.yOffset + size_t(y) * destination.yStride,
                job->output.data() + size_t(y) * input.width, input.width);
    for (uint32_t y = 0; y < input.height / 2; ++y) {
        uint8_t *row = destination.data + destination.uvOffset + size_t(y) * destination.uvStride;
        const uint8_t *output = job->output.data() + ySize + size_t(y) * input.width;
        if (!nv12) memcpy(row, output, input.width);
        else {
            uint32_t x = 0;
#if defined(__ARM_NEON)
            for (; x + 16 <= input.width; x += 16)
                vst1q_u8(row + x, vrev16q_u8(vld1q_u8(output + x)));
#endif
            for (; x < input.width; x += 2) {
                row[x] = output[x + 1];
                row[x + 1] = output[x];
            }
        }
    }
    recycleJobLocked(*job);
    return 0;
}

void QCamera3PreviewBokehWorker::run() {
    pthread_setname_np(pthread_self(), "OCamBokeh");
    std::unique_lock<std::mutex> lock(mMutex);
    while (!mCancelled) {
        Frame auxiliary;
        int32_t pairStatus = -EAGAIN;
        mCondition.wait(lock, [&] {
            if (mCancelled) return true;
            if (mJob) pairStatus = pairLocked(*mJob, auxiliary);
            return pairStatus != -EAGAIN;
        });
        if (mCancelled) break;
        auto job = mJob;
        job->auxiliaryMetadata = auxiliary.metadata;
        mJob.reset();
        if (pairStatus != 0) {
            job->status = pairStatus;
            job->done = true;
            mCondition.notify_all();
            continue;
        }
        mActiveJob = job;
        selectFacesLocked(*job);
        const bool reset = mResetRequired;
        mResetRequired = false;
        job->output.swap(mOutputPixels);
        lock.unlock();
        const Image wide = image(job->main, mConfig.imageWidths[0], mConfig.imageHeights[0]);
        const Image tele = image(auxiliary, mConfig.imageWidths[1], mConfig.imageHeights[1]);
        {
            KPI_ATRACE_NAME("ocam.bokeh.worker.allocateOut");
            job->output.resize(job->main.pixels.size());
        }
        const Image output = {job->output.data(), job->output.size(), wide.width, wide.height,
            0, size_t(wide.width) * wide.height, wide.width, wide.width, wide.height, wide.height / 2};
        const Backend::Pair pair = {{job->main.source.cameraHandle, auxiliary.source.cameraHandle},
            {job->main.source.nativeFrameNumber, auxiliary.source.nativeFrameNumber},
            {job->main.metadata.exposureStartNs, auxiliary.metadata.exposureStartNs},
            job->main.metadata.lensShiftUm, job->main.metadata.afRegion,
            job->faceFrame, job->faceCount, job->faceStatus, job->selectedFace};
        int32_t status = reset ? mBackend.configure(mConfig, mCalibration) : 0;
        const QCamera3PreviewBokehGeometry::Frame geometry[] = {
            job->main.source.geometry, auxiliary.source.geometry};
        const bool mapped = geometry[0].width || geometry[1].width;
        if (status == 0) status = mBackend.process(pair, wide, tele, output,
                job->focus, job->result, &job->cancelled, mapped ? geometry : nullptr);
        lock.lock();
        job->status = status;
        job->done = true;
        mActiveJob.reset();
        recyclePixelsLocked(auxiliary.pixels, mAuxiliaryPixels);
        if (job->cancelled || mCancelled) recycleJobLocked(*job);
        mCondition.notify_all();
    }
}

void QCamera3PreviewBokehWorker::cancel() {
    std::lock_guard<std::mutex> lock(mMutex);
    mCancelled = true;
    if (mJob) {
        mJob->cancelled = true;
        recycleJobLocked(*mJob);
    }
    if (mActiveJob) mActiveJob->cancelled = true;
    mCondition.notify_all();
}

void QCamera3PreviewBokehWorker::flush() {
    std::lock_guard<std::mutex> lock(mMutex);
    if (mJob) {
        mJob->cancelled = true;
        recycleJobLocked(*mJob);
    }
    if (mActiveJob) mActiveJob->cancelled = true;
    mJob.reset();
    for (auto &frame : mAuxiliary) recyclePixelsLocked(frame.pixels, mAuxiliaryPixels);
    mAuxiliary.clear();
    mMetadata[0].clear(); mMetadata[1].clear();
    mFaces.clear(); mFaceSelector = {};
    mCondition.notify_all();
}

void QCamera3PreviewBokehWorker::resetTemporalState() {
    std::lock_guard<std::mutex> lock(mMutex);
    mResetRequired = true;
    mFaces.clear(); mFaceSelector = {};
}

void QCamera3PreviewBokehWorker::stop() {
    cancel();
    if (mThread.joinable()) mThread.join();
    std::lock_guard<std::mutex> lock(mMutex);
    mRunning = false;
    mJob.reset();
    mActiveJob.reset();
    mAuxiliary.clear();
    mMetadata[0].clear(); mMetadata[1].clear();
    mFaces.clear(); mFaceSelector = {};
    mSessionIds[0] = mSessionIds[1] = 0;
    mBackend.reset();
    std::vector<uint8_t>().swap(mMainPixels);
    std::vector<uint8_t>().swap(mAuxiliaryPixels);
    std::vector<uint8_t>().swap(mOutputPixels);
}
} // namespace qcamera
