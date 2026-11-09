/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_OPPO_VISIDON_PANORAMA_H
#define QCAMERA3_OPPO_VISIDON_PANORAMA_H

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <string>

namespace qcamera {

/*
 * Front-camera selfie panorama backend: in-process driver for the stock OPPO
 * Visidon engine (`/vendor/lib/libVDSelfiePanorama.so`, 32-bit JNI library,
 * task-152/153). The HAL3 camera provider process is 32-bit, so the library is
 * loaded with an absolute path and driven through a fake JNIEnv
 * (visidon_jnienv.h); no JVM, no IPC.
 *
 * Pure C++ / host testable: the vendor library is only dlopen'ed in
 * init() when Config::testEngine is null (host tests inject EngineAbi).
 *
 * Sequence: init() -> addFrame() x N (N >= minFrames, engine MAX_FRAMES=3) ->
 * process()/captureJpeg()/captureNv21() -> cancel()/deinit(). The engine output
 * is NV21 (1440x930 for a 1440x1080 input) and JPEG; JPEG rotation follows
 * Config::rotationDeg and is applied by the engine (probe: 90/270 => 930x1440).
 * The caller (HWI, task-157) owns stream plumbing and BLOB delivery.
 *
 * Error contract: every call returns a Status; HAL panorama failure reasons
 * (0-13, task-153 sec 4) are mapped by halFailureReason(). No fake image is
 * produced on failure.
 */
class QCamera3OppoVisidonPanorama {
public:
    enum Status : int32_t {
        OK = 0,
        E_STATE = -1,    // call out of order
        E_PARAM = -2,    // bad config/frame/rotation/output buffer
        E_NOMEM = -3,    // memory/output-size guard
        E_LIMIT = -4,    // maxFrames already fed
        E_FRAMES = -5,   // fewer than minFrames accepted at capture
        E_ENCODE = -6,   // engine process/JPEG step failed
        E_CANCELED = -7, // canceled
        E_LOAD = -8,     // vendor library load/init or license failure
        E_INTERNAL = -9, // unexpected engine/JNI behavior
        E_JPEG = -10,    // engine JPEG encode failed
    };

    enum class State {
        Idle,
        Inited,
        Capturing,
        Ready,
        Canceled,
        Failed,
    };

    // Engine ABI exactly as reverse-engineered from the stock library
    // (Java_visidon_Lib_Panorama_*; probe-verified on R11s).
    struct EngineAbi {
        long long (*init)(void *env, void *thiz, int width, int height, int focalPx,
                int saveInput) = nullptr;
        int (*addYuvFrame)(void *env, void *thiz, void *nv21Array, int width, int height,
                int frameDelay, long long engine) = nullptr;
        int (*addFrame)(void *env, void *thiz, void *yBuffer, void *uBuffer,
                void *vBuffer, int width, int height, int stride, long long engine) = nullptr;
        void *(*process)(void *env, void *thiz, long long engine) = nullptr;
        void *(*getNv21)(void *env, void *thiz, long long engine) = nullptr;
        void *(*getJpeg)(void *env, void *thiz, int outWidth, int rotation, long long engine) = nullptr;
        void (*cancelProcessing)(void *env, void *thiz, long long engine) = nullptr;
        void (*release)(void *env, void *thiz, long long engine) = nullptr;

        bool valid() const {
            return init != nullptr && addYuvFrame != nullptr && process != nullptr &&
                    getJpeg != nullptr && release != nullptr;
        }
    };

    struct Config {
        uint32_t previewWidth = 1440;
        uint32_t previewHeight = 1080;
        int32_t focalPx = 1085;
        int32_t rotationDeg = 0;      // 0/90/180/270, applied by getJPEG
        uint32_t minFrames = 3;       // stock MAX_FRAMES = 3
        uint32_t maxFrames = 3;       // max accepted addFrame calls
        size_t maxFrameBytes = 0;     // 0 => previewWidth*previewHeight*3/2
        size_t maxOutputBytes = 16u * 1024u * 1024u;
        std::string libPath = "/vendor/lib/libVDSelfiePanorama.so";
        // Test-only injection. Production must leave this null so init() loads
        // the vendor library with an absolute path.
        const EngineAbi *testEngine = nullptr;
    };

    // One NV21 frame from the HAL preview stream (any stride >= width).
    struct Frame {
        const uint8_t *y = nullptr;
        const uint8_t *uv = nullptr;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t yStride = 0;
        uint32_t uvStride = 0;
        int64_t timestampUs = 0;
    };

    // Caller-provided output buffer; the backend never allocates for output.
    struct Image {
        uint8_t *data = nullptr;
        size_t capacity = 0;
        size_t length = 0;
        uint32_t width = 0;
        uint32_t height = 0;
    };

    QCamera3OppoVisidonPanorama();
    ~QCamera3OppoVisidonPanorama();
    QCamera3OppoVisidonPanorama(const QCamera3OppoVisidonPanorama &) = delete;
    QCamera3OppoVisidonPanorama &operator=(const QCamera3OppoVisidonPanorama &) = delete;

    Status init(const Config &config);
    Status reinit(const Config &config);
    Status addFrame(const Frame &frame);
    Status process(uint32_t *outWidth, uint32_t *outHeight);
    Status captureJpeg(Image *output);
    Status captureNv21(Image *output);
    Status cancel();
    void deinit();

    State state() const;
    uint32_t acceptedFrames() const;
    uint32_t outputWidth() const;
    uint32_t outputHeight() const;
    const Config &config() const;

    // task-153 sec 4 failure-reason mapping: 0/1/3/7/8/9/10/12/13.
    static int32_t halFailureReason(Status status);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace qcamera

#endif  // QCAMERA3_OPPO_VISIDON_PANORAMA_H
