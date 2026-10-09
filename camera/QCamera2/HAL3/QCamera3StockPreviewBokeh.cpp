/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3StockPreviewBokeh.h"
#include "QCamera3StockStBokehAbi.h"
#if defined(__ANDROID__)
#include "QCamera3AlgorithmCpuScope.h"
#endif
#include <dlfcn.h>
#include <errno.h>
#include <new>
#include <stdlib.h>
#include <string.h>
#include <chrono>
#if defined(__ANDROID__)
#include "QCameraTrace.h"
#else
#define KPI_ATRACE_NAME(name) ((void)0)
#endif

namespace qcamera {
namespace {
bool hasCurrentFrameContract(void *library) {
    const auto code = [&](const char *symbol) {
        return reinterpret_cast<const uint8_t *>(
                reinterpret_cast<uintptr_t>(dlsym(library, symbol)) & ~uintptr_t(1));
    };
    const uint8_t *blur = code("_ZN12StereoEngine10stereoBlurEPhS0_S0_ii13stStereoEnv_tPii");
    const uint8_t *process = code("rtbdoBokehEffect");
    const uint8_t renderCurrent[] = {0x00, 0xf0, 0x76, 0xba};
    const uint8_t returnState[] = {0x33, 0x98};
    return blur && process && !memcmp(blur + 0x8fe, renderCurrent, sizeof(renderCurrent)) &&
            !memcmp(process + 0x19c, returnState, sizeof(returnState)) &&
            !memcmp(process + 0x1c0, returnState, sizeof(returnState));
}
struct Api {
    void *library = dlopen("libSTDualCamPreviewBokeh.so", RTLD_NOW | RTLD_LOCAL);
    stockst::Api calls;
    std::mutex mutex;
    void *owner = nullptr;
    bool available = calls.load(library) && hasCurrentFrameContract(library);
};
Api &api() {
    static Api instance;
    return instance;
}
constexpr uint64_t kScratchLimit = 64U * 1024U * 1024U;
bool stopped(const std::atomic<bool> *flag) { return flag && flag->load(); }
void rows(uint8_t *dst, uint32_t dstStride, const uint8_t *src, uint32_t srcStride,
        uint32_t width, uint32_t height) {
    for (uint32_t y = 0; y < height; ++y)
        memcpy(dst + size_t(y) * dstStride, src + size_t(y) * srcStride, width);
}
bool validConfig(const QCamera3StockPreviewBokeh::Config &c) {
    if (!c.cameraHandles[0] || !c.cameraHandles[1] || c.cameraHandles[0] == c.cameraHandles[1] ||
            c.imageWidths[0] != c.imageWidths[1] || c.imageHeights[0] != c.imageHeights[1] ||
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
    void *handle = nullptr;
    Config config = {};
    stockst::Image images[3] = {};
    stockst::Stream streams[3] = {};
    uint8_t *pixels[3] = {};
    std::vector<uint8_t> referencePixels;
    QCamera3PreviewBokehGeometry mapping[2];
    uint8_t calibration[512] = {};
    bool ownsEngine = false;
    ~Impl() {
        Api &backend = api();
        std::lock_guard<std::mutex> lock(backend.mutex);
        if (handle) backend.calls.uninitialize(&handle);
        if (ownsEngine) backend.owner = nullptr;
        for (auto *image : pixels) free(image);
    }
};

QCamera3StockPreviewBokeh::QCamera3StockPreviewBokeh() = default;
QCamera3StockPreviewBokeh::~QCamera3StockPreviewBokeh() = default;

bool QCamera3StockPreviewBokeh::isAvailable() {
    return api().available;
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
    Api &backend = api();
    if (!backend.available) return -ENOSYS;
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
        impl->streams[i] = {int32_t(width), int32_t(height), int32_t(stride), int32_t(height)};
        stockst::setNv21Layout(impl->images[i], impl->streams[i], impl->pixels[i],
                impl->pixels[i] + size_t(stride) * height);
    }
    if (uint64_t(impl->streams[0].stride) * impl->streams[0].height * 3 / 2 >
            kScratchLimit - total) return -EOVERFLOW;
    memcpy(impl->calibration, calibration.data, sizeof(impl->calibration));
    const auto dimension = [&](size_t offset) {
        int16_t value;
        memcpy(&value, impl->calibration + offset, sizeof(value));
        return int32_t(value);
    };
    const int32_t widths[] = {dimension(0xba), dimension(0xbe)};
    const int32_t heights[] = {dimension(0xb8), dimension(0xbc)};
    for (unsigned i = 0; i < 2; ++i) {
        if (widths[i] <= 0 || heights[i] <= 0 ||
                config.activeWidths[i] > uint32_t(widths[i]) ||
                config.activeHeights[i] > uint32_t(heights[i])) return -ENOTSUP;
    }
    const stockst::Calibration blob = {impl->calibration, sizeof(impl->calibration)};
    const stockst::CameraImageInfo geometry = {
        stockst::centeredGeometry(widths[0], heights[0], config.activeWidths[0], config.activeHeights[0]),
        stockst::centeredGeometry(widths[1], heights[1], config.activeWidths[1], config.activeHeights[1])};
    int32_t rc;
    {
        // The stock DSO shares its engine and environment across all handles.
        std::lock_guard<std::mutex> lock(backend.mutex);
        if (backend.owner) return -EBUSY;
        backend.owner = impl.get();
        impl->ownsEngine = true;
        rc = backend.calls.initialize(&impl->handle, &impl->streams[0],
                &impl->streams[1], &impl->streams[2]);
        if (rc != 0) impl->handle = nullptr; // ST releases its engine on initialization failure.
        if (rc == 0 && !impl->handle) rc = -EIO;
        if (rc == 0) rc = backend.calls.calibration(impl->handle, &blob);
        if (rc == 0) rc = backend.calls.geometry(impl->handle, &geometry);
        if (rc == 0) rc = backend.calls.rotation(impl->handle, config.degree);
    }
    if (rc != 0) return rc < 0 ? rc : -EIO;
    mImpl = std::move(impl);
    return 0;
}

int32_t QCamera3StockPreviewBokeh::process(const Pair &pair, const Image &wide,
        const Image &tele, const Image &destination, const Focus &focus, Result &result,
        const std::atomic<bool> *cancelled,
        const QCamera3PreviewBokehGeometry::Frame *geometry) {
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
            (!focus.sensorCoordinates && !((focus.x == -1 && focus.y == -1) ||
                (focus.x >= 0 && focus.y >= 0 && uint32_t(focus.x) < wide.width &&
                 uint32_t(focus.y) < wide.height)))) return -EINVAL;
    if (stopped(cancelled)) return -ECANCELED;
    if (focus.blurLevel == 0) return -ENODATA;
#if defined(__ANDROID__)
    // ST's background workers are created by configure, before this caller-only scope.
    QCamera3AlgorithmCpuScope cpuScope;
    KPI_ATRACE_INT("ocam.bokeh.cpuScope", cpuScope.status());
#endif
    const auto geometryBegin = std::chrono::steady_clock::now();
    if (geometry) {
        for (unsigned i = 0; i < 2; ++i) {
            if (geometry[i].width != impl.config.imageWidths[i] ||
                    geometry[i].height != impl.config.imageHeights[i] ||
                    geometry[i].referenceWindow.width != impl.config.activeWidths[i] ||
                    geometry[i].referenceWindow.height != impl.config.activeHeights[i] ||
                    uint64_t(impl.config.activeWidths[i]) * impl.config.imageHeights[i] !=
                            uint64_t(impl.config.activeHeights[i]) * impl.config.imageWidths[i]) return -EINVAL;
            const int32_t status = impl.mapping[i].configure(geometry[i]);
            if (status != 0) return status;
        }
        if (!focus.sensorCoordinates) return -ERANGE;
    } else if (focus.sensorCoordinates) return -EINVAL;
    const auto &roi = pair.afRegion;
    const QCamera3PreviewBokehGeometry::Rect reference = geometry ? geometry[0].referenceWindow :
            QCamera3PreviewBokehGeometry::Rect{0, 0, double(wide.width), double(wide.height)};
    if (!std::isfinite(roi.left) || !std::isfinite(roi.top) ||
            !std::isfinite(roi.width) || !std::isfinite(roi.height) ||
            roi.width <= 0 || roi.height <= 0) return -ENODATA;
    const double left = std::max(roi.left, reference.left);
    const double top = std::max(roi.top, reference.top);
    const double right = std::min(roi.left + roi.width, reference.left + reference.width);
    const double bottom = std::min(roi.top + roi.height, reference.top + reference.height);
    if (right <= left || bottom <= top) return -ENODATA;
    // Oppo keeps AF left/top and uses the smaller scaled side, without recentering.
    const int32_t x = (left - reference.left) * wide.width / reference.width;
    const int32_t y = (top - reference.top) * wide.height / reference.height;
    const int32_t side = std::min<int32_t>((right - left) * wide.width / reference.width,
            (bottom - top) * wide.height / reference.height);
    if (side <= 0) return -ENODATA;
    const stockst::Refocus params = {x, y, side, side, 0, 0, focus.blurLevel};
    result.refocusRoi[0] = x; result.refocusRoi[1] = y;
    result.refocusRoi[2] = side; result.refocusRoi[3] = side;
    for (unsigned i = 0; i < 2; ++i) {
        KPI_ATRACE_NAME(i ? "ocam.bokeh.reference.aux" : "ocam.bokeh.reference.main");
        const Image &source = *inputs[i];
        auto &dst = impl.images[i];
        if (geometry && !impl.mapping[i].identity()) {
            const Image reference = {dst.y, size_t(dst.stride) * dst.height * 3 / 2,
                uint32_t(dst.width), uint32_t(dst.height), 0,
                size_t(dst.stride) * dst.height, uint32_t(dst.stride),
                uint32_t(dst.stride), uint32_t(dst.height), uint32_t(dst.height / 2)};
            const int32_t status = impl.mapping[i].referenceImage(source, reference);
            if (status != 0) return status;
        } else {
            rows(dst.y, dst.stride, source.data + source.yOffset, source.yStride,
                    source.width, source.height);
            rows(dst.chroma, dst.stride, source.data + source.uvOffset, source.uvStride,
                    source.width, source.height / 2);
        }
    }
    const bool projectMain = geometry && !impl.mapping[0].identity();
    if (projectMain) {
        const size_t length = size_t(impl.images[0].stride) * wide.height * 3 / 2;
        impl.referencePixels.resize(length);
        memcpy(impl.referencePixels.data(), impl.pixels[0], length);
    }
    if (geometry) {
        result.geometryPrepareNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - geometryBegin).count();
    }
    // A core success without any output writes must remain a passthrough result.
    for (unsigned plane = 0; plane < 2; ++plane)
        rows((plane ? impl.images[2].chroma : impl.images[2].y), impl.images[2].stride,
                (plane ? impl.images[0].chroma : impl.images[0].y), impl.images[0].stride,
                wide.width, plane ? wide.height / 2 : wide.height);
    stockst::Image images[3];
    memcpy(images, impl.images, sizeof(images));
    images[0].metadata.lensShiftUm = pair.mainLensShiftUm;
    if (pair.faceCount && pair.faceFrameNumber == pair.frameNumbers[0]) {
        const auto &face = pair.selectedFace;
        const double faceLeft = std::max(face.left, reference.left);
        const double faceTop = std::max(face.top, reference.top);
        const double faceRight = std::min(face.left + face.width, reference.left + reference.width);
        const double faceBottom = std::min(face.top + face.height, reference.top + reference.height);
        if (faceRight > faceLeft && faceBottom > faceTop) {
            const int32_t rectangle[] = {
                int32_t((faceLeft - reference.left) * wide.width / reference.width),
                int32_t((faceTop - reference.top) * wide.height / reference.height),
                int32_t((faceRight - faceLeft) * wide.width / reference.width),
                int32_t((faceBottom - faceTop) * wide.height / reference.height)};
            if (rectangle[2] > 0 && rectangle[3] > 0) {
                images[0].metadata.faceCount = pair.faceCount;
                memcpy(images[0].metadata.selectedFace, rectangle, sizeof(rectangle));
                memcpy(result.faceRoi, rectangle, sizeof(rectangle));
            }
        }
    }
    if (stopped(cancelled)) return -ECANCELED;
    const auto coreBegin = std::chrono::steady_clock::now();
    {
        Api &backend = api();
        std::lock_guard<std::mutex> lock(backend.mutex);
        KPI_ATRACE_NAME("ocam.bokeh.core");
        result.coreStatus = backend.calls.inputs(impl.handle, &images[0], &images[1]);
        if (result.coreStatus == 0)
            result.coreStatus = backend.calls.process(impl.handle, &images[0], &images[2],
                    nullptr, &params, &result.effectStatus);
    }
    result.coreProcessNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - coreBegin).count();
    if (stopped(cancelled)) return -ECANCELED;
    if (result.coreStatus == 0 || result.coreStatus == 6) return -ENODATA;
    if (result.coreStatus != 1) return result.coreStatus < 0 ? result.coreStatus : -EPROTO;
    images[0].metadata = {};
    if (memcmp(images, impl.images, sizeof(images)) != 0) return -EPROTO;
    bool changed = false;
    for (unsigned p = 0; p < 2 && !changed; ++p) {
        const uint8_t *source = projectMain ? impl.referencePixels.data() +
                (p ? size_t(impl.images[0].stride) * wide.height : 0) :
                wide.data + (p ? wide.uvOffset : wide.yOffset);
        const uint32_t stride = projectMain ? impl.images[0].stride :
                (p ? wide.uvStride : wide.yStride);
        for (uint32_t y = 0; y < (p ? wide.height / 2 : wide.height); ++y)
            if (memcmp(source + size_t(y) * stride,
                    (p ? images[2].chroma : images[2].y) + size_t(y) * images[2].stride, wide.width) != 0) {
                changed = true;
                break;
            }
    }
    if (!changed) return -ENODATA;
    if (projectMain) {
        const auto projectBegin = std::chrono::steady_clock::now();
        const Image reference = {impl.referencePixels.data(), impl.referencePixels.size(),
            wide.width, wide.height, 0, size_t(impl.images[0].stride) * wide.height,
            uint32_t(impl.images[0].stride), uint32_t(impl.images[0].stride),
            wide.height, wide.height / 2};
        const Image processed = {images[2].y,
            size_t(images[2].stride) * wide.height * 3 / 2, wide.width, wide.height, 0,
            size_t(images[2].stride) * wide.height, uint32_t(images[2].stride),
            uint32_t(images[2].stride), wide.height, wide.height / 2};
        const Image projected = {impl.pixels[0], reference.length, wide.width, wide.height,
            0, reference.uvOffset, reference.yStride, reference.uvStride,
            wide.height, wide.height / 2};
        int32_t status;
        {
            KPI_ATRACE_NAME("ocam.bokeh.project.main");
            status = impl.mapping[0].project(wide, reference, processed, projected, changed);
        }
        result.geometryProjectNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - projectBegin).count();
        if (status != 0) return status;
        if (!changed) return -ENODATA;
        if (stopped(cancelled)) return -ECANCELED;
        rows(destination.data + destination.yOffset, destination.yStride,
                projected.data, projected.yStride, wide.width, wide.height);
        rows(destination.data + destination.uvOffset, destination.uvStride,
                projected.data + projected.uvOffset, projected.uvStride, wide.width, wide.height / 2);
        result.applied = true;
        return 0;
    }
    rows(destination.data + destination.yOffset, destination.yStride,
            images[2].y, images[2].stride, wide.width, wide.height);
    rows(destination.data + destination.uvOffset, destination.uvStride,
            images[2].chroma, images[2].stride, wide.width, wide.height / 2);
    result.applied = true;
    return 0;
}

} // namespace qcamera
