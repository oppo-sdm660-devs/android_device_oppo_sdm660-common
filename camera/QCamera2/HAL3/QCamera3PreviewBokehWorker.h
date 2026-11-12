/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_PREVIEW_BOKEH_WORKER_H
#define QCAMERA3_PREVIEW_BOKEH_WORKER_H

#include "QCamera3StockPreviewBokeh.h"
#include <condition_variable>
#include <deque>
#include <map>
#include <thread>
#include <vector>
#include <errno.h>

namespace qcamera {

// The driver callback keeps its output; this worker only owns CPU copies.
class QCamera3PreviewBokehWorker {
public:
    using Backend = QCamera3StockPreviewBokeh;
    using Image = Backend::Image;
    struct Source { uint32_t cameraHandle, frameNumber; int64_t timestampNs; };
    QCamera3PreviewBokehWorker() = default;
    ~QCamera3PreviewBokehWorker();
    int32_t configure(const Backend::Config &, const Backend::Calibration &, int64_t timeoutNs);
    void metadata(const Source &);
    int32_t auxiliary(const Source &, const Image &, bool nv12);
    int32_t process(const Source &, const Image &, bool nv12, const Image &destination,
            const Backend::Focus &, Backend::Result &);
    void cancel();
    void flush();
    void resetTemporalState();
    void stop();

private:
    struct Frame { Source source; std::vector<uint8_t> pixels; };
    struct Job {
        Frame main;
        Backend::Focus focus;
        Backend::Result result;
        std::vector<uint8_t> output;
        std::atomic<bool> cancelled{false};
        bool done = false;
        int32_t status = -EAGAIN;
    };
    static Image image(Frame &, uint32_t width, uint32_t height);
    static bool copy(const Image &, bool nv12, Frame &);
    void run();
    bool pairLocked(Job &, Frame &);
    Backend mBackend;
    Backend::Config mConfig = {};
    Backend::Calibration mCalibration;
    int64_t mTimeoutNs = 0;
    std::mutex mMutex;
    std::condition_variable mCondition;
    std::map<uint32_t, int64_t> mMetadata[2];
    std::deque<Frame> mAuxiliary;
    std::shared_ptr<Job> mJob;
    std::shared_ptr<Job> mActiveJob;
    std::thread mThread;
    bool mRunning = false;
    bool mResetRequired = false;
    std::atomic<bool> mCancelled{false};
};

} // namespace qcamera
#endif
