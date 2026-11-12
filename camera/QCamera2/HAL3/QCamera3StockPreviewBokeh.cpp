/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3StockPreviewBokeh.h"
#include <dlfcn.h>
#include <errno.h>
#include <new>
#include <stdlib.h>
#include <string.h>

namespace qcamera {
namespace {
struct ArcImage {
    uint32_t format;
    int32_t width, height;
    uint8_t *planes[4];
    int32_t pitches[4];
};
struct ArcCalibration { uint8_t *data; int32_t length; };
struct ArcGeometry { int32_t wideWidth, wideHeight, teleWidth, teleHeight; };
struct ArcParam {
    int32_t focusX, focusY, blurLevel, defaultMode;
    void *faceRects, *faceOrientations;
    int32_t faceCount, reserved;
};
#if UINTPTR_MAX == UINT32_MAX
static_assert(sizeof(ArcImage) == 44, "ARC_DCVR ARM32 image ABI");
static_assert(sizeof(ArcCalibration) == 8, "ARC_DCVR ARM32 calibration ABI");
static_assert(sizeof(ArcParam) == 32, "ARC_DCVR ARM32 preview parameter ABI");
#endif
static_assert(sizeof(ArcGeometry) == 16, "ARC_DCVR geometry ABI");
struct Api {
    void *library = dlopen("libarcsoft_dualcam_refocus_preview.so", RTLD_NOW | RTLD_LOCAL);
    int32_t (*init)(void **) = nullptr;
    int32_t (*uninit)(void **) = nullptr;
    int32_t (*calibration)(void *, ArcCalibration *) = nullptr;
    int32_t (*geometry)(void *, ArcGeometry *) = nullptr;
    int32_t (*degree)(void *, int32_t) = nullptr;
    int32_t (*defaults)(ArcParam *) = nullptr;
    int32_t (*process)(void *, ArcImage *, ArcImage *, ArcImage *, ArcParam *, int32_t *) = nullptr;
    Api() {
        if (!library) return;
#define LOAD(member, symbol) member = reinterpret_cast<decltype(member)>(dlsym(library, symbol))
        LOAD(init, "ARC_DCVR_Init");
        LOAD(uninit, "ARC_DCVR_Uninit");
        LOAD(calibration, "ARC_DCVR_SetCaliData");
        LOAD(geometry, "ARC_DCVR_SetCameraImageInfo");
        LOAD(degree, "ARC_DCVR_SetImageDegree");
        LOAD(defaults, "ARC_DCVR_GetDefaultParam");
        LOAD(process, "ARC_DCVR_Process");
#undef LOAD
    }
    ~Api() { if (library) dlclose(library); }
    bool valid() const {
        return init && uninit && calibration && geometry && degree && defaults && process;
    }
};
constexpr uint64_t kScratchLimit = 64U * 1024U * 1024U;
bool stopped(const std::atomic<bool> *flag) { return flag && flag->load(); }
void rows(uint8_t *dst, uint32_t dstStride, const uint8_t *src, uint32_t srcStride,
        uint32_t width, uint32_t height) {
    for (uint32_t y = 0; y < height; ++y)
        memcpy(dst + size_t(y) * dstStride, src + size_t(y) * srcStride, width);
}
bool validConfig(const QCamera3StockPreviewBokeh::Config &c) {
    if (!c.cameraHandles[0] || !c.cameraHandles[1] || c.cameraHandles[0] == c.cameraHandles[1] ||
            c.maxTimestampDeltaNs <= 0 || c.maxTimestampDeltaNs > 10000000 ||
            (c.degree != 0 && c.degree != 90 && c.degree != 180 && c.degree != 270)) return false;
    for (unsigned i = 0; i < 2; ++i) {
        if (!c.imageWidths[i] || !c.imageHeights[i] ||
                ((c.imageWidths[i] | c.imageHeights[i]) & 1) ||
                c.activeWidths[i] < c.imageWidths[i] || c.activeHeights[i] < c.imageHeights[i] ||
                c.activeWidths[i] > 16384 || c.activeHeights[i] > 16384) return false;
    }
    return true;
}
} // namespace

struct QCamera3StockPreviewBokeh::Impl {
    Api api;
    void *handle = nullptr;
    Config config = {};
    ArcParam defaults = {};
    ArcImage images[3] = {};
    uint8_t *pixels[3] = {};
    uint8_t calibration[512] = {};
    ~Impl() {
        if (handle) api.uninit(&handle);
        for (auto *image : pixels) free(image);
    }
};

QCamera3StockPreviewBokeh::QCamera3StockPreviewBokeh() = default;
QCamera3StockPreviewBokeh::~QCamera3StockPreviewBokeh() = default;

bool QCamera3StockPreviewBokeh::isAvailable() {
    Api api;
    return api.valid();
}

void QCamera3StockPreviewBokeh::reset() {
    std::lock_guard<std::mutex> lock(mMutex);
    mImpl.reset();
}

int32_t QCamera3StockPreviewBokeh::configure(const Config &config, const Calibration &calibration) {
    std::lock_guard<std::mutex> lock(mMutex);
    mImpl.reset();
    if (!validConfig(config) || calibration.source == Calibration::NONE ||
            !QCamera3DualBokehAlgo::validCalibration(calibration.data, sizeof(calibration.data)))
        return -EINVAL;
    std::unique_ptr<Impl> impl(new (std::nothrow) Impl);
    if (!impl) return -ENOMEM;
    if (!impl->api.valid()) return -ENOSYS;
    impl->config = config;
    uint64_t total = 0;
    for (unsigned i = 0; i < 3; ++i) {
        const unsigned input = i == 1 ? 1 : 0;
        const uint32_t width = config.imageWidths[input], height = config.imageHeights[input];
        const uint32_t stride = (width + 63) & ~63U;
        const uint64_t length = uint64_t(stride) * height * 3 / 2;
        total += length;
        if (length > SIZE_MAX || total > kScratchLimit) return -EOVERFLOW;
        impl->pixels[i] = static_cast<uint8_t *>(calloc(1, size_t(length)));
        if (!impl->pixels[i]) return -ENOMEM;
        impl->images[i] = {0x802, int32_t(width), int32_t(height),
                {impl->pixels[i], impl->pixels[i] + size_t(stride) * height, nullptr, nullptr},
                {int32_t(stride), int32_t(stride), 0, 0}};
    }
    memcpy(impl->calibration, calibration.data, sizeof(impl->calibration));
    ArcCalibration blob = {impl->calibration, sizeof(impl->calibration)};
    ArcGeometry geometry = {int32_t(config.activeWidths[0]), int32_t(config.activeHeights[0]),
            int32_t(config.activeWidths[1]), int32_t(config.activeHeights[1])};
    int32_t rc = impl->api.init(&impl->handle);
    if (rc == 0 && !impl->handle) rc = -EIO;
    if (rc == 0) rc = impl->api.calibration(impl->handle, &blob);
    if (rc == 0) rc = impl->api.geometry(impl->handle, &geometry);
    if (rc == 0) rc = impl->api.degree(impl->handle, config.degree);
    // The exported default writer sets only the first 16 bytes; face/reserved
    // fields remain zero and must not contain uninitialized stack data.
    if (rc == 0) rc = impl->api.defaults(&impl->defaults);
    if (rc != 0) return rc;
    mImpl = std::move(impl);
    return 0;
}

int32_t QCamera3StockPreviewBokeh::process(const Pair &pair, const Image &wide,
        const Image &tele, const Image &destination, const Focus &focus, Result &result,
        const std::atomic<bool> *cancelled) {
    std::lock_guard<std::mutex> lock(mMutex);
    result = Result{};
    result.source = pair;
    if (!mImpl) return -ENODEV;
    Impl &impl = *mImpl;
    const Image *inputs[] = {&wide, &tele, &destination};
    for (unsigned i = 0; i < 3; ++i) {
        const unsigned input = i == 1 ? 1 : 0;
        if (!QCamera3HdrAlgo::validImage(*inputs[i]) ||
                inputs[i]->width != impl.config.imageWidths[input] ||
                inputs[i]->height != impl.config.imageHeights[input]) return -EINVAL;
    }
    if (pair.cameraHandles[0] != impl.config.cameraHandles[0] ||
            pair.cameraHandles[1] != impl.config.cameraHandles[1] ||
            pair.timestampsNs[0] <= 0 || pair.timestampsNs[1] <= 0 ||
            (pair.timestampsNs[0] > pair.timestampsNs[1] ?
                pair.timestampsNs[0] - pair.timestampsNs[1] :
                pair.timestampsNs[1] - pair.timestampsNs[0]) > impl.config.maxTimestampDeltaNs ||
            focus.blurLevel < 0 || focus.blurLevel > 100 ||
            !((focus.x == -1 && focus.y == -1) || (focus.x >= 0 && focus.y >= 0 &&
                uint32_t(focus.x) < wide.width && uint32_t(focus.y) < wide.height))) return -EINVAL;
    if (stopped(cancelled)) return -ECANCELED;
    if (focus.blurLevel == 0) return -ENODATA;
    for (unsigned i = 0; i < 2; ++i) {
        const Image &source = *inputs[i];
        auto &dst = impl.images[i];
        rows(dst.planes[0], dst.pitches[0], source.data + source.yOffset, source.yStride,
                source.width, source.height);
        rows(dst.planes[1], dst.pitches[1], source.data + source.uvOffset, source.uvStride,
                source.width, source.height / 2);
    }
    // A core success without any output writes must remain a passthrough result.
    for (unsigned plane = 0; plane < 2; ++plane)
        rows(impl.images[2].planes[plane], impl.images[2].pitches[plane],
                impl.images[0].planes[plane], impl.images[0].pitches[plane],
                wide.width, plane ? wide.height / 2 : wide.height);
    ArcParam params = impl.defaults;
    params.focusX = focus.x;
    params.focusY = focus.y;
    params.blurLevel = focus.blurLevel;
    ArcImage images[3];
    memcpy(images, impl.images, sizeof(images));
    result.coreStatus = impl.api.process(impl.handle, &images[0], &images[1], &images[2],
            &params, &result.effectStatus);
    if (stopped(cancelled)) return -ECANCELED;
    if (result.coreStatus != 0) return result.coreStatus;
    if (memcmp(images, impl.images, sizeof(images)) != 0) return -EPROTO;
    bool changed = false;
    for (unsigned p = 0; p < 2 && !changed; ++p) {
        const uint8_t *source = wide.data + (p ? wide.uvOffset : wide.yOffset);
        const uint32_t stride = p ? wide.uvStride : wide.yStride;
        for (uint32_t y = 0; y < (p ? wide.height / 2 : wide.height); ++y)
            if (memcmp(source + size_t(y) * stride,
                    images[2].planes[p] + size_t(y) * images[2].pitches[p], wide.width) != 0) {
                changed = true;
                break;
            }
    }
    if (!changed) return -ENODATA;
    rows(destination.data + destination.yOffset, destination.yStride,
            images[2].planes[0], images[2].pitches[0], wide.width, wide.height);
    rows(destination.data + destination.uvOffset, destination.uvStride,
            images[2].planes[1], images[2].pitches[1], wide.width, wide.height / 2);
    result.applied = true;
    return 0;
}

} // namespace qcamera
