/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3StockScene.h"
#include <algorithm>
#include <cmath>
#include <dlfcn.h>
#include <errno.h>
#include <new>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

namespace qcamera {
namespace {
// R11/R11s ASD_SceneDetector ASVLOFFSCREEN ABI: 44 bytes on ARM32, orientation
// at arg-3 offset 0, reset flag at 4; 28-byte output (count 0, types 4, ratings 16).
struct Offscreen {
    uint32_t format;
    int32_t width, height;
    uint8_t *planes[4];
    int32_t pitches[4];
};
struct Parameters { uint16_t orientation, reserved; int32_t handshake; };
struct Detection { int32_t count, types[3]; float ratings[3]; };
static_assert(sizeof(Parameters) == 8 && sizeof(Detection) == 28, "ASD argument ABI");
#if UINTPTR_MAX == UINT32_MAX
static_assert(sizeof(Offscreen) == 44, "ASD ARM32 image ABI");
#endif
pthread_mutex_t gSceneLock = PTHREAD_MUTEX_INITIALIZER;
struct Lock {
    Lock() { pthread_mutex_lock(&gSceneLock); }
    ~Lock() { pthread_mutex_unlock(&gSceneLock); }
};
struct Api {
    void *library = dlopen("libarcsoft_aiscenedetection.so", RTLD_NOW | RTLD_LOCAL);
    int32_t (*init)(void *, void **) = nullptr;
    int32_t (*detect)(void *, Offscreen *, Parameters *, Detection *) = nullptr;
    void (*uninit)(void *) = nullptr;
    Api() {
        if (!library) return;
        init = reinterpret_cast<decltype(init)>(dlsym(library, "ASD_Init"));
        detect = reinterpret_cast<decltype(detect)>(dlsym(library, "ASD_SceneDetector"));
        uninit = reinterpret_cast<decltype(uninit)>(dlsym(library, "ASD_UnInit"));
    }
    ~Api() { if (library) dlclose(library); }
    bool valid() const { return init && detect && uninit; }
};
struct Buffer {
    uint8_t *data;
    explicit Buffer(size_t bytes) : data(static_cast<uint8_t *>(malloc(bytes))) {}
    ~Buffer() { free(data); }
};
bool stopped(const std::atomic<bool> *flag) { return flag && flag->load(); }
uint8_t clip(int value) { return uint8_t(std::max(0, std::min(255, value))); }
// Pre-tuning sample incl. chroma; invalidates stale chains after a cut/pan.
void signature(const QCamera3HdrAlgo::Image &image, uint8_t out[192]) {
    for (uint32_t y = 0; y < 8; ++y) for (uint32_t x = 0; x < 16; ++x)
        out[y * 16 + x] = image.data[image.yOffset +
            size_t((2 * y + 1) * image.height / 16) * image.yStride +
            (2 * x + 1) * image.width / 32];
    for (uint32_t y = 0; y < 4; ++y) for (uint32_t x = 0; x < 8; ++x) {
        const uint32_t col = ((2 * x + 1) * image.width / 16) & ~1U;
        const uint8_t *uv = image.data + image.uvOffset +
            size_t((2 * y + 1) * (image.height / 2) / 8) * image.uvStride + col;
        out[128 + (y * 8 + x) * 2] = uv[0];
        out[129 + (y * 8 + x) * 2] = uv[1];
    }
}
// HAL3 tuning: scene-dependent SDR curves/chroma gains, not stock SCES values.
void tuning(int32_t type, int &contrast, int &lift, int &saturation, int &warmth) {
    contrast = lift = warmth = 0; saturation = 100;
    switch (type) {
    case 1: case 21: case 47: contrast = 3; saturation = 110; break;
    case 2: case 20: lift = 2; saturation = 101; break;
    case 4: lift = 4; saturation = 97; break;
    case 8: saturation = 107; warmth = 3; break;
    case 9: saturation = 105; contrast = 2; break;
    case 10: lift = 2; saturation = 98; break;
    case 11: contrast = 4; saturation = 107; break;
    case 12: contrast = 5; saturation = 97; break;
    case 19: contrast = 2; saturation = 108; warmth = 1; break;
    case 22: lift = 2; saturation = 102; break;
    case 31: case 32: contrast = 2; saturation = 104; break;
    case 38: contrast = 2; saturation = 101; break;
    default: break; // Normal and unknown scenes are identity.
    }
}
}
struct QCamera3StockScene::Engine {
    Api api;
    void *handle = nullptr;
    ~Engine() { if (handle && api.uninit) api.uninit(handle); }
};
QCamera3StockScene::~QCamera3StockScene() { reset(); }
void QCamera3StockScene::reset() {
    Lock lock; delete mEngine; mEngine = nullptr; mHaveCache = false; mCached = {};
    mWidth = mHeight = 0;
}
bool QCamera3StockScene::isAvailable() { Lock lock; Api api; return api.valid(); }
bool QCamera3StockScene::knownType(int32_t type) {
    switch (type) {
    case 0: case 1: case 2: case 4: case 8: case 9: case 10: case 11:
    case 12: case 19: case 20: case 21: case 22: case 31: case 32: case 38: case 47:
        return true;
    default: return false;
    }
}
bool QCamera3StockScene::supports(uint32_t width, uint32_t height) {
    const uint32_t small = std::min(width, height), large = std::max(width, height);
    return small >= 32 && !(width & 1) && !(height & 1) &&
        uint64_t(large) <= uint64_t(small) * 4 &&
        uint64_t(width) * height <= 32U * 1024U * 1024U;
}
int32_t QCamera3StockScene::process(const Image &image, int32_t strength,
        uint32_t number, int64_t nowNs, Result &result, const std::atomic<bool> *cancelled) {
    result = {}; result.frameNumber = result.sourceFrame = number; result.sourceNs = nowNs;
    const auto fail = [this, &result](int32_t status) {
        mHaveCache = false;result.status = status;result.applied = 0;return status;
    };
    if (strength < 0 || strength > 100 || !supports(image.width, image.height) ||
            !QCamera3HdrAlgo::validImage(image) || nowNs <= 0) return fail(-EINVAL);
    if (!strength) { mHaveCache = false; return 0; }
    if (stopped(cancelled)) return fail(-ECANCELED);
    uint8_t current[192]; signature(image, current);
    unsigned change = 0;
    for (unsigned i = 0; i < sizeof(current); ++i)
        change += unsigned(std::abs(int(current[i]) - int(mSignature[i])));
    const bool reusable = mHaveCache && mWidth == image.width && mHeight == image.height &&
        number >= mCached.sourceFrame && number - mCached.sourceFrame <= 4 &&
        nowNs >= mCached.sourceNs && nowNs - mCached.sourceNs <= 200000000LL &&
        change <= 18 * sizeof(current);
    if (reusable) {
        result = mCached; result.frameNumber = number;result.ageFrames = number - result.sourceFrame;
    } else {
        mHaveCache = false;
        // Core needs a >=192 short edge and <=4:1 aspect; input is at most 768x192
        // NV21 (~216 KiB) independent of the still size; the model uses 192x192.
        const uint32_t small = std::min(image.width, image.height);
        uint32_t width = uint32_t((uint64_t(image.width) * 192 + small - 1) / small + 1) & ~1U;
        uint32_t height = uint32_t((uint64_t(image.height) * 192 + small - 1) / small + 1) & ~1U;
        const size_t pixels = size_t(width) * height;
        Buffer analysis{pixels * 3 / 2};
        if (!analysis.data) return fail(-ENOMEM);
        for (uint32_t y = 0; y < height; ++y) {
            if (stopped(cancelled)) return fail(-ECANCELED);
            const uint8_t *src = image.data + image.yOffset +
                size_t(uint64_t(y) * image.height / height) * image.yStride;
            for (uint32_t x = 0; x < width; ++x)
                analysis.data[size_t(y) * width + x] = src[uint64_t(x) * image.width / width];
        }
        for (uint32_t y = 0; y < height / 2; ++y) {
            if (stopped(cancelled)) return fail(-ECANCELED);
            const uint8_t *src = image.data + image.uvOffset +
                size_t(uint64_t(y) * (image.height / 2) / (height / 2)) * image.uvStride;
            for (uint32_t x = 0; x < width; x += 2) {
                const uint32_t col = uint32_t(uint64_t(x) * image.width / width) & ~1U;
                analysis.data[pixels + size_t(y) * width + x] = src[col];
                analysis.data[pixels + size_t(y) * width + x + 1] = src[col + 1];
            }
        }
        Offscreen input = {0x802, int32_t(width), int32_t(height),
            {analysis.data, analysis.data + pixels, nullptr, nullptr},
            {int32_t(width), int32_t(width), 0, 0}};
        Parameters parameters = {}; Detection detected = {};
        int32_t status = 0;
        {
            Lock lock;
            if (!mEngine) mEngine = new (std::nothrow) Engine;
            if (!mEngine) return fail(-ENOMEM);
            if (!mEngine->api.valid()) return fail(-ENOSYS);
            if (!mEngine->handle) {
                // The core's allocator (0x53ffc/0x540e0) picks malloc/free for a null manager.
                status = mEngine->api.init(nullptr, &mEngine->handle);
                if (status || !mEngine->handle) status = -EIO;
            }
            if (!status) status = stopped(cancelled) ? -ECANCELED :
                mEngine->api.detect(mEngine->handle, &input, &parameters, &detected);
            if (!status && (input.format != 0x802 || input.width != int32_t(width) ||
                    input.height != int32_t(height) || input.planes[0] != analysis.data ||
                    input.planes[1] != analysis.data + pixels || input.planes[2] || input.planes[3] ||
                    input.pitches[0] != int32_t(width) || input.pitches[1] != int32_t(width) ||
                    input.pitches[2] || input.pitches[3])) status = -EPROTO;
            if (status) { delete mEngine; mEngine = nullptr; }
        }
        if (status) return fail(status < 0 ? status : -EIO);
        if (stopped(cancelled)) return fail(-ECANCELED);
        if (detected.count < 0 || detected.count > 3) return fail(-EPROTO);
        float best = -1.0f; int32_t type = -1;
        for (int i = 0; i < detected.count; ++i) {
            const float score = detected.ratings[i];
            if (!std::isfinite(score) || score < 0.0f || score > 1.0f)
                return fail(-EPROTO);
            if (score > best) { best = score; type = detected.types[i]; }
        }
        result.type = knownType(type) ? type : -1;
        result.confidence = best < 0 ? 0 : int32_t(best * 1000.0f + 0.5f);
        mCached = result; mWidth = image.width; mHeight = image.height;
        memcpy(mSignature, current, sizeof(current)); mHaveCache = detected.count > 0;
    }
    if (result.type < 0 || result.confidence < 600 || result.type == 0) return 0;
    int contrast, lift, saturation, warmth;
    tuning(result.type, contrast, lift, saturation, warmth);
    const size_t pixels = size_t(image.width) * image.height;
    Buffer output{pixels * 3 / 2};
    if (!output.data) return fail(-ENOMEM);
    for (uint32_t y = 0; y < image.height; ++y) {
        if (stopped(cancelled)) return fail(-ECANCELED);
        const uint8_t *src = image.data + image.yOffset + size_t(y) * image.yStride;
        for (uint32_t x = 0; x < image.width; ++x) {
            const int value = src[x], studio = std::max(0, std::min(219, value - 16));
            const int delta = contrast * (studio - 110) / 100 + lift * (219 - studio) / 219;
            output.data[size_t(y) * image.width + x] = clip(value + delta * strength / 100);
        }
    }
    for (uint32_t y = 0; y < image.height / 2; ++y) {
        if (stopped(cancelled)) return fail(-ECANCELED);
        const uint8_t *src = image.data + image.uvOffset + size_t(y) * image.uvStride;
        uint8_t *dst = output.data + pixels + size_t(y) * image.width;
        for (uint32_t x = 0; x < image.width; x += 2) {
            dst[x] = clip(src[x] + ((int(src[x]) - 128) * (saturation - 100) / 100 + warmth) * strength / 100);
            dst[x + 1] = clip(src[x + 1] + ((int(src[x + 1]) - 128) * (saturation - 100) / 100 - warmth) * strength / 100);
        }
    }
    if (stopped(cancelled)) return fail(-ECANCELED);
    for (uint32_t y = 0; y < image.height; ++y)
        memcpy(image.data + image.yOffset + size_t(y) * image.yStride,
            output.data + size_t(y) * image.width, image.width);
    for (uint32_t y = 0; y < image.height / 2; ++y)
        memcpy(image.data + image.uvOffset + size_t(y) * image.uvStride,
            output.data + pixels + size_t(y) * image.width, image.width);
    result.applied = strength;
    return 0;
}
}
