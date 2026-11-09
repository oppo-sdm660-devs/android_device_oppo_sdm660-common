/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Front-camera Visidon selfie-panorama backend (task-156).
 *
 * Pure C++/host-testable driver for the stock 32-bit JNI engine
 * libVDSelfiePanorama.so. The vendor library is loaded with an absolute path
 * only inside init() when Config::testEngine is null; host tests inject the ABI.
 *
 * Stock engine facts used here (task-152/153, probe-verified on R11s):
 *  - nativeInit(width, height, focalPx, saveInput) -> engine (0 = failure);
 *  - nativeAddYUVFrame(byte[] NV21, width, height, frameDelay, engine) -> 0 ok;
 *  - nativeProcess(engine) -> int[2] = [outWidth, outHeight] (1440,930);
 *  - nativeGetJPEGImage(engine) via nativeGetJPEGImage(outW, rotation, engine)
 *    where rotation is 0/90/180/270 (90/270 swap output dimensions);
 *  - nativeGetNV21Image(engine) -> NV21 of outW*outH*3/2;
 *  - nativeRelease(engine); engine MAX_FRAMES = 3.
 */
#include "QCamera3OppoVisidonPanorama.h"

#include <dlfcn.h>

#include <cstdio>
#include <new>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

#include "visidon_jnienv.h"

namespace qcamera {

namespace {

constexpr uint32_t kMaxDimension = 16384;
constexpr uint32_t kMinStockFrames = 3; // stock Panorama.MAX_FRAMES

bool mulOverflow(size_t a, size_t b, size_t *out) {
    if (a != 0 && b > SIZE_MAX / a) return true;
    *out = a * b;
    return false;
}

bool normalizeRotation(int32_t rotation, int32_t *out) {
    if (rotation == 0 || rotation == 90 || rotation == 180 || rotation == 270) {
        *out = rotation;
        return true;
    }
    return false;
}

bool loadVendorLibrary(const std::string &path, QCamera3OppoVisidonPanorama::EngineAbi *api,
        void **handle) {
    // Absolute path only: the HAL process namespace must not rely on the shell
    // LD_LIBRARY_PATH setup used by the device probe.
    if (path.empty() || path[0] != '/') return false;
    void *lib = dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (lib == nullptr) {
        std::fprintf(stderr, "visidon: dlopen(%s) failed: %s\n", path.c_str(), dlerror());
        return false;
    }
    auto load = [lib](const char *name) {
        void *sym = dlsym(lib, name);
        return sym;
    };
    api->init = reinterpret_cast<decltype(api->init)>(
            load("Java_visidon_Lib_Panorama_nativeInit"));
    api->addYuvFrame = reinterpret_cast<decltype(api->addYuvFrame)>(
            load("Java_visidon_Lib_Panorama_nativeAddYUVFrame"));
    api->addFrame = reinterpret_cast<decltype(api->addFrame)>(
            load("Java_visidon_Lib_Panorama_nativeAddFrame"));
    api->process = reinterpret_cast<decltype(api->process)>(
            load("Java_visidon_Lib_Panorama_nativeProcess"));
    api->getNv21 = reinterpret_cast<decltype(api->getNv21)>(
            load("Java_visidon_Lib_Panorama_nativeGetNV21Image"));
    api->getJpeg = reinterpret_cast<decltype(api->getJpeg)>(
            load("Java_visidon_Lib_Panorama_nativeGetJPEGImage"));
    api->cancelProcessing = reinterpret_cast<decltype(api->cancelProcessing)>(
            load("Java_visidon_Lib_Panorama_nativeCancelProcessing"));
    api->release = reinterpret_cast<decltype(api->release)>(
            load("Java_visidon_Lib_Panorama_nativeRelease"));
    if (!api->valid()) {
        std::fprintf(stderr, "visidon: %s missing native symbols\n", path.c_str());
        dlclose(lib);
        return false;
    }
    *handle = lib;
    return true;
}

}  // namespace

class QCamera3OppoVisidonPanorama::Impl {
public:
    mutable std::mutex lock;
    Config config;
    State state = State::Idle;
    VisidonJniEnv *env = nullptr;
    EngineAbi api;
    void *dlHandle = nullptr;
    long long engine = 0;
    uint32_t accepted = 0;
    uint32_t outWidth = 0;
    uint32_t outHeight = 0;
    std::vector<uint8_t> packed;

    bool slotFault() {
        return env != nullptr && env->takeSlotError();
    }

    Status validate(const Config &cfg) {
        int32_t rotation = 0;
        if (!normalizeRotation(cfg.rotationDeg, &rotation)) return E_PARAM;
        if (cfg.previewWidth == 0 || cfg.previewHeight == 0 ||
                (cfg.previewWidth & 1u) || (cfg.previewHeight & 1u) ||
                cfg.previewWidth > kMaxDimension || cfg.previewHeight > kMaxDimension)
            return E_PARAM;
        if (cfg.focalPx <= 0) return E_PARAM;
        if (cfg.minFrames < kMinStockFrames || cfg.minFrames > cfg.maxFrames) return E_PARAM;
        if (cfg.maxFrames > 8) return E_PARAM;
        if (!cfg.testEngine && !cfg.libPath.empty() && cfg.libPath[0] != '/') return E_PARAM;
        size_t frameBytes = 0;
        if (mulOverflow(cfg.previewWidth, cfg.previewHeight, &frameBytes)) return E_NOMEM;
        if (mulOverflow(frameBytes, 3, &frameBytes) || frameBytes > SIZE_MAX / 2) return E_NOMEM;
        frameBytes /= 2;
        if (cfg.maxFrameBytes != 0 && cfg.maxFrameBytes < frameBytes) return E_NOMEM;
        return OK;
    }

    Status loadEngine() {
        const Config &cfg = config;
        if (cfg.testEngine != nullptr) {
            api = *cfg.testEngine;
            if (!api.valid()) return E_LOAD;
        } else if (!loadVendorLibrary(cfg.libPath, &api, &dlHandle)) {
            return E_LOAD;
        }
        env = new (std::nothrow) VisidonJniEnv();
        if (env == nullptr) return E_NOMEM;
        engine = api.init(env->jniEnv(), VisidonJniEnv::fakeThis(),
                int(cfg.previewWidth), int(cfg.previewHeight), cfg.focalPx, 0);
        if (slotFault()) {
            if (dlHandle != nullptr) {
                dlclose(dlHandle);
                dlHandle = nullptr;
            }
            api = EngineAbi();
            engine = 0;
            delete env;
            env = nullptr;
            return E_INTERNAL;
        }
        if (engine == 0) return E_LOAD;
        return OK;
    }

    void releaseEngine() {
        if (engine != 0 && api.release != nullptr)
            api.release(env != nullptr ? env->jniEnv() : nullptr,
                    VisidonJniEnv::fakeThis(), engine);
        engine = 0;
        api = EngineAbi();
        if (dlHandle != nullptr) {
            dlclose(dlHandle);
            dlHandle = nullptr;
        }
        delete env;
        env = nullptr;
        packed.clear();
        packed.shrink_to_fit();
        accepted = 0;
        outWidth = 0;
        outHeight = 0;
    }

    Status processLocked() {
        if (state != State::Capturing) return E_STATE;
        void *result = api.process(env->jniEnv(), VisidonJniEnv::fakeThis(), engine);
        auto *dims = static_cast<VisidonJniEnv::IntArrayOut *>(result);
        if (slotFault()) {
            VisidonJniEnv::freeOutput(dims);
            return E_INTERNAL;
        }
        if (dims == nullptr || dims->data == nullptr || dims->length < 2 ||
                dims->data[0] <= 0 || dims->data[1] <= 0) {
            VisidonJniEnv::freeOutput(dims);
            return E_INTERNAL;
        }
        outWidth = uint32_t(dims->data[0]);
        outHeight = uint32_t(dims->data[1]);
        VisidonJniEnv::freeOutput(dims);
        return OK;
    }

    Status fail(Status status) {
        releaseEngine();
        state = State::Failed;
        return status;
    }
};

QCamera3OppoVisidonPanorama::QCamera3OppoVisidonPanorama() : impl_(new Impl()) {}

QCamera3OppoVisidonPanorama::~QCamera3OppoVisidonPanorama() {
    deinit();
}

QCamera3OppoVisidonPanorama::Status QCamera3OppoVisidonPanorama::init(const Config &config) {
    std::lock_guard<std::mutex> guard(impl_->lock);
    impl_->releaseEngine();
    impl_->state = State::Idle;
    impl_->config = config;
    const Status valid = impl_->validate(config);
    if (valid != OK) {
        impl_->state = State::Failed;
        return valid;
    }
    const Status loaded = impl_->loadEngine();
    if (loaded != OK) {
        impl_->releaseEngine();
        impl_->state = State::Failed;
        return loaded;
    }
    impl_->accepted = 0;
    impl_->outWidth = 0;
    impl_->outHeight = 0;
    impl_->state = State::Capturing;
    return OK;
}

QCamera3OppoVisidonPanorama::Status QCamera3OppoVisidonPanorama::reinit(const Config &config) {
    deinit();
    return init(config);
}

QCamera3OppoVisidonPanorama::Status QCamera3OppoVisidonPanorama::addFrame(const Frame &frame) {
    std::lock_guard<std::mutex> guard(impl_->lock);
    Impl &s = *impl_;
    if (s.state != State::Capturing) return E_STATE;
    if (s.accepted >= s.config.maxFrames) return E_LIMIT;
    if (frame.y == nullptr || frame.uv == nullptr || frame.width == 0 ||
            frame.height == 0)
        return E_PARAM;
    if (frame.width != s.config.previewWidth || frame.height != s.config.previewHeight ||
            (frame.width & 1u) || (frame.height & 1u))
        return E_PARAM;
    if (frame.yStride < frame.width || frame.uvStride < frame.width) return E_PARAM;
    const uint64_t ySize64 = uint64_t(frame.width) * frame.height;
    const uint64_t uvSize64 = uint64_t(frame.width) * (frame.height / 2);
    const uint64_t total64 = ySize64 + uvSize64;
    if (ySize64 > SIZE_MAX || uvSize64 > SIZE_MAX || total64 > SIZE_MAX) return E_NOMEM;
    const size_t ySize = size_t(ySize64);
    const size_t total = size_t(total64);
    const size_t limit = s.config.maxFrameBytes != 0 ? s.config.maxFrameBytes :
            (size_t(frame.width) * frame.height * 3 / 2);
    if (total > limit) return E_NOMEM;
    s.packed.resize(total);
    for (uint32_t row = 0; row < frame.height; row++)
        std::memcpy(s.packed.data() + size_t(row) * frame.width,
                frame.y + size_t(row) * frame.yStride, frame.width);
    for (uint32_t row = 0; row < frame.height / 2; row++)
        std::memcpy(s.packed.data() + ySize + size_t(row) * frame.width,
                frame.uv + size_t(row) * frame.uvStride, frame.width);
    VisidonJniEnv::ByteArrayIn in;
    in.data = s.packed.data();
    in.size = s.packed.size();
    const int rc = s.api.addYuvFrame(s.env->jniEnv(), VisidonJniEnv::fakeThis(), &in,
            int(frame.width), int(frame.height), 0, s.engine);
    if (s.slotFault()) return impl_->fail(E_INTERNAL);
    if (rc != 0) return impl_->fail(E_INTERNAL);
    s.accepted++;
    return OK;
}

QCamera3OppoVisidonPanorama::Status QCamera3OppoVisidonPanorama::process(
        uint32_t *outWidth, uint32_t *outHeight) {
    std::lock_guard<std::mutex> guard(impl_->lock);
    if (outWidth == nullptr || outHeight == nullptr) return E_PARAM;
    const Status rc = impl_->processLocked();
    if (rc != OK) return rc;
    *outWidth = impl_->outWidth;
    *outHeight = impl_->outHeight;
    return OK;
}

QCamera3OppoVisidonPanorama::Status QCamera3OppoVisidonPanorama::captureJpeg(Image *output) {
    std::lock_guard<std::mutex> guard(impl_->lock);
    Impl &s = *impl_;
    if (output == nullptr || output->data == nullptr || output->capacity == 0) return E_PARAM;
    if (s.state != State::Capturing) return E_STATE;
    if (s.accepted < s.config.minFrames) return impl_->fail(E_FRAMES);
    Status rc = impl_->processLocked();
    if (rc != OK) return impl_->fail(rc);
    int32_t rotation = 0;
    if (!normalizeRotation(s.config.rotationDeg, &rotation)) return impl_->fail(E_PARAM);
    void *result = s.api.getJpeg(s.env->jniEnv(), VisidonJniEnv::fakeThis(),
            int(s.outWidth), rotation, s.engine);
    auto *jpeg = static_cast<VisidonJniEnv::ByteArrayOut *>(result);
    if (s.slotFault()) {
        VisidonJniEnv::freeOutput(jpeg);
        return impl_->fail(E_INTERNAL);
    }
    if (jpeg == nullptr || jpeg->data == nullptr || jpeg->length == 0) {
        VisidonJniEnv::freeOutput(jpeg);
        return impl_->fail(E_JPEG);
    }
    if (jpeg->length > s.config.maxOutputBytes) {
        VisidonJniEnv::freeOutput(jpeg);
        return impl_->fail(E_NOMEM);
    }
    if (jpeg->length > output->capacity) {
        // Caller-provided buffer too small: retryable, do not destroy the engine.
        VisidonJniEnv::freeOutput(jpeg);
        return E_NOMEM;
    }
    std::memcpy(output->data, jpeg->data, jpeg->length);
    output->length = jpeg->length;
    if (rotation == 90 || rotation == 270) {
        output->width = s.outHeight;
        output->height = s.outWidth;
    } else {
        output->width = s.outWidth;
        output->height = s.outHeight;
    }
    VisidonJniEnv::freeOutput(jpeg);
    s.state = State::Ready;
    return OK;
}

QCamera3OppoVisidonPanorama::Status QCamera3OppoVisidonPanorama::captureNv21(Image *output) {
    std::lock_guard<std::mutex> guard(impl_->lock);
    Impl &s = *impl_;
    if (output == nullptr || output->data == nullptr || output->capacity == 0) return E_PARAM;
    if (s.state != State::Capturing) return E_STATE;
    if (s.accepted < s.config.minFrames) return impl_->fail(E_FRAMES);
    Status rc = impl_->processLocked();
    if (rc != OK) return impl_->fail(rc);
    if (s.api.getNv21 == nullptr) return impl_->fail(E_LOAD);
    void *result = s.api.getNv21(s.env->jniEnv(), VisidonJniEnv::fakeThis(), s.engine);
    auto *nv21 = static_cast<VisidonJniEnv::ByteArrayOut *>(result);
    if (s.slotFault()) {
        VisidonJniEnv::freeOutput(nv21);
        return impl_->fail(E_INTERNAL);
    }
    if (nv21 == nullptr || nv21->data == nullptr) {
        VisidonJniEnv::freeOutput(nv21);
        return impl_->fail(E_JPEG);
    }
    const uint64_t expected64 = uint64_t(s.outWidth) * s.outHeight * 3 / 2;
    if (expected64 == 0 || expected64 > SIZE_MAX) {
        VisidonJniEnv::freeOutput(nv21);
        return impl_->fail(E_NOMEM);
    }
    const size_t expected = size_t(expected64);
    if (nv21->length < expected || nv21->length > s.config.maxOutputBytes) {
        VisidonJniEnv::freeOutput(nv21);
        return impl_->fail(E_NOMEM);
    }
    if (nv21->length > output->capacity) {
        VisidonJniEnv::freeOutput(nv21);
        return E_NOMEM;
    }
    std::memcpy(output->data, nv21->data, nv21->length);
    output->length = nv21->length;
    output->width = s.outWidth;
    output->height = s.outHeight;
    VisidonJniEnv::freeOutput(nv21);
    s.state = State::Ready;
    return OK;
}

QCamera3OppoVisidonPanorama::Status QCamera3OppoVisidonPanorama::cancel() {
    std::lock_guard<std::mutex> guard(impl_->lock);
    if (impl_->engine != 0 && impl_->api.cancelProcessing != nullptr) {
        impl_->api.cancelProcessing(impl_->env->jniEnv(), VisidonJniEnv::fakeThis(),
                impl_->engine);
    }
    impl_->releaseEngine();
    impl_->state = State::Canceled;
    return OK;
}

void QCamera3OppoVisidonPanorama::deinit() {
    std::lock_guard<std::mutex> guard(impl_->lock);
    impl_->releaseEngine();
    impl_->state = State::Idle;
}

QCamera3OppoVisidonPanorama::State QCamera3OppoVisidonPanorama::state() const {
    std::lock_guard<std::mutex> guard(impl_->lock);
    return impl_->state;
}

uint32_t QCamera3OppoVisidonPanorama::acceptedFrames() const {
    std::lock_guard<std::mutex> guard(impl_->lock);
    return impl_->accepted;
}

uint32_t QCamera3OppoVisidonPanorama::outputWidth() const {
    std::lock_guard<std::mutex> guard(impl_->lock);
    return impl_->outWidth;
}

uint32_t QCamera3OppoVisidonPanorama::outputHeight() const {
    std::lock_guard<std::mutex> guard(impl_->lock);
    return impl_->outHeight;
}

const QCamera3OppoVisidonPanorama::Config &QCamera3OppoVisidonPanorama::config() const {
    return impl_->config;
}

int32_t QCamera3OppoVisidonPanorama::halFailureReason(Status status) {
    switch (status) {
        case OK: return 0;
        case E_LOAD: return 1;       // UNSUPPORTED
        case E_FRAMES: return 3;     // INSUFFICIENT_FRAMES
        case E_PARAM: return 7;      // FRAME_INVALID / INVALID_REQUEST
        case E_ENCODE: return 8;     // STITCH_FAILED
        case E_JPEG: return 9;       // JPEG_ENCODE_FAILED
        case E_NOMEM: return 10;     // OUT_OF_MEMORY
        case E_CANCELED: return 12;  // CANCELLED
        default: return 13;          // INTERNAL
    }
}

}  // namespace qcamera
