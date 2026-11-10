/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_PANORAMA_WORKER_H
#define QCAMERA3_PANORAMA_WORKER_H

#include "QCamera3OppoPanorama.h"

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace qcamera {

class QCamera3PanoramaWorker {
public:
    enum class State { Idle, Initializing, Capturing, Finalizing, Completed, Canceled, Failed };
    enum Status : int32_t { OK = 0, E_BUSY = -100, E_JPEG = -101 };

    struct Config {
        QCamera3OppoPanorama::Config panorama;
        size_t maxPendingFrames = 2; // includes the frame currently being processed
        size_t maxPendingBytes = 16u * 1024u * 1024u;
        size_t maxOutputBytes = 32u * 1024u * 1024u;
        int64_t minFrameIntervalNs = 250000000;
    };

    struct Source {
        uint32_t frameNumber = 0;
        int64_t timestampNs = 0;
    };

    struct Frame {
        const uint8_t *data = nullptr;
        size_t length = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        size_t yOffset = 0;
        size_t uvOffset = 0;
        uint32_t yStride = 0;
        uint32_t uvStride = 0;
        Source source;
        QCamera3OppoPanorama::GyroSample gyro;
    };

    struct Result {
        // Opaque HWI internal frame; its orchestration map owns framework identity.
        uint32_t requestFrame = 0;
        int32_t status = QCamera3OppoPanorama::E_STATE;
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<Source> sources;
        std::vector<uint8_t> jpeg;
    };

    struct Stats {
        State state = State::Idle;
        uint32_t acceptedFrames = 0;
        uint32_t droppedFrames = 0;
        uint32_t rejectedFrames = 0;
        size_t pendingFrames = 0;
        size_t pendingBytes = 0;
        size_t peakPendingBytes = 0;
        int32_t lastStatus = OK;
    };

    using Completion = std::function<void(Result)>;

    QCamera3PanoramaWorker();
    ~QCamera3PanoramaWorker();
    QCamera3PanoramaWorker(const QCamera3PanoramaWorker &) = delete;
    QCamera3PanoramaWorker &operator=(const QCamera3PanoramaWorker &) = delete;

    // Init, frame processing and JPEG encoding all run on the worker thread.
    int32_t start(const Config &config);
    // Copies visible NV21 rows before returning; the camera buffer stays caller-owned.
    // Busy/interval-limited inputs are dropped instead of waiting for the backend.
    int32_t submit(const Frame &frame);
    // Drains accepted frames, then completes exactly once, including cancel/failure.
    int32_t requestJpeg(uint32_t requestFrame, uint32_t quality,
            size_t maxJpegBytes, Completion completion);
    void cancel();
    // Flush/close barrier, including completion callbacks. Call outside HWI locks.
    // A completion runs on this worker and must not call this barrier or destroy it.
    void cancelAndDrain();
    Stats stats() const;
    // Source identities whose pixels are queued, running, or retained in the canvas.
    std::vector<Source> retainedSources() const;

private:
    struct OwnedFrame {
        std::vector<uint8_t> bytes;
        Source source;
        QCamera3OppoPanorama::GyroSample gyro;
    };
    void run();
    Result finish(QCamera3OppoPanorama &panorama, uint32_t requestFrame,
            uint32_t quality, size_t maxJpegBytes);
    void complete(Result result, Completion completion);
    static bool validFrame(const Frame &frame);

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable drained_;
    std::thread thread_;
    Config config_;
    Stats stats_;
    std::deque<OwnedFrame> frames_;
    std::vector<Source> sources_;
    Source workingSource_;
    bool hasWorkingSource_ = false;
    Completion completion_;
    uint32_t requestFrame_ = 0;
    uint32_t quality_ = 95;
    size_t maxJpegBytes_ = 0;
    int64_t lastEnqueuedTimestampNs_ = 0;
    bool hasEnqueuedTimestamp_ = false;
    bool initRequested_ = false;
    bool finishRequested_ = false;
    bool cancelRequested_ = false;
    bool working_ = false;
    bool stopping_ = false;
};

} // namespace qcamera
#endif
