/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_PREVIEW_BOKEH_WORKER_H
#define QCAMERA3_PREVIEW_BOKEH_WORKER_H

#include "QCamera3StockPreviewBokeh.h"
#include "QCamera3PreviewBokehFaces.h"
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
    struct Source {
        uint32_t cameraHandle, nativeFrameNumber;
        int64_t imageSofNs; // Legacy name: ISP AXI_DONE buffer timestamp, for diagnostics only.
        int32_t geometryStatus = 0;
        QCamera3PreviewBokehGeometry::Frame geometry = {};
    };
    // Raw metadata buffer time can be wall time; exposureStartNs is BOOTTIME.
    struct Metadata {
        uint32_t cameraHandle, nativeFrameNumber, requestFrameNumber;
        int64_t rawTimestampNs, exposureStartNs;
        int32_t status = 0;
        float lensShiftUm = 0;
        QCamera3PreviewBokehGeometry::Rect afRegion = {};
    };
    struct Origin { Metadata metadata[2] = {}; };
    QCamera3PreviewBokehWorker() = default;
    ~QCamera3PreviewBokehWorker();
    int32_t configure(const Backend::Config &, const Backend::Calibration &, int64_t timeoutNs,
            const uint32_t *sessionIds = nullptr);
    void metadata(const Metadata &);
    void faces(const QCamera3PreviewBokehFaces::Observation &);
    int32_t auxiliary(const Source &, const Image &, bool nv12);
    int32_t process(uint32_t requestFrameNumber, const Source &, const Image &, bool nv12,
            const Image &destination, const Backend::Focus &, Backend::Result &, Origin * = nullptr);
    void cancel();
    void flush();
    void resetTemporalState();
    void stop();

private:
    struct Frame { Source source; Metadata metadata = {}; std::vector<uint8_t> pixels; };
    struct Job {
        Frame main;
        Metadata auxiliaryMetadata = {};
        uint32_t requestFrameNumber;
        Backend::Focus focus;
        Backend::Result result;
        uint32_t faceFrame = 0, faceCount = 0;
        int32_t faceStatus = 0;
        QCamera3PreviewBokehGeometry::Rect selectedFace = {};
        std::vector<uint8_t> output;
        std::atomic<bool> cancelled{false};
        bool done = false;
        int32_t status = -EAGAIN;
    };
    static Image image(Frame &, uint32_t width, uint32_t height);
    static bool copy(const Image &, bool nv12, Frame &);
    static void recyclePixelsLocked(std::vector<uint8_t> &, std::vector<uint8_t> &);
    void recycleJobLocked(Job &);
    void run();
    int32_t pairLocked(Job &, Frame &);
    void selectFacesLocked(Job &);
    Backend mBackend;
    Backend::Config mConfig = {};
    Backend::Calibration mCalibration;
    int64_t mTimeoutNs = 0;
    std::mutex mMutex;
    std::condition_variable mCondition;
    std::map<uint32_t, Metadata> mMetadata[2];
    uint32_t mSessionIds[2] = {};
    std::map<uint32_t, QCamera3PreviewBokehFaces::Observation> mFaces;
    QCamera3PreviewBokehFaces mFaceSelector;
    std::deque<Frame> mAuxiliary;
    std::shared_ptr<Job> mJob;
    std::shared_ptr<Job> mActiveJob;
    // One idle allocation per role; active jobs retain their pixels through cancellation.
    std::vector<uint8_t> mMainPixels, mAuxiliaryPixels, mOutputPixels;
    std::thread mThread;
    bool mRunning = false;
    bool mResetRequired = false;
    std::atomic<bool> mCancelled{false};
};

} // namespace qcamera
#endif
