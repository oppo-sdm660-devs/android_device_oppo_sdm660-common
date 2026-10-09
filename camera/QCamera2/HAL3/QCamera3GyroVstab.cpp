/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3GyroVstab.h"

#include <cmath>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <new>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

namespace qcamera {
namespace {

constexpr unsigned kGyroHistory = 256;
constexpr int64_t kMaxGyroGapNs = 500000000;
constexpr int64_t kMaxInWindowGapNs = 20000000;
constexpr int64_t kMaxFrameGapNs = 1000000000;
constexpr float kBiasAlpha = 0.02f;
constexpr float kPanAlpha = 0.25f;
constexpr float kMaxPanRad = 0.5f;
constexpr float kMaxBiasRadPerSec = 1.0f;
constexpr unsigned kCancelRows = 64;
constexpr int kCoordinateBits = 24;
constexpr int64_t kCoordinateScale = int64_t(1) << kCoordinateBits;
constexpr uint32_t kMaxGrid = 65;
constexpr int64_t kMaxProfileBytes = 65536;
constexpr double kMaxAngleRad = 0.5;

// Stock core ABI; entry points are the undecorated exports of libmms_gyro_vstab.so.
struct Vertex { float x, y, z; };
struct CoreFrame {
    uint32_t size, revision;
    int64_t timestampNs, exposureNs;
    float zoom, gain;
    int32_t rotation;
    void *debugImage;
};
struct CoreOutput { uint32_t size, revision; Vertex *vertices; };
static_assert(sizeof(Vertex) == 12, "MMS vertex ABI");
#if UINTPTR_MAX == UINT32_MAX
static_assert(sizeof(CoreFrame) == 40, "MMS ARM32 frame ABI");
static_assert(offsetof(CoreFrame, timestampNs) == 8, "MMS frame timestamp ABI");
static_assert(offsetof(CoreFrame, debugImage) == 36, "MMS frame image ABI");
static_assert(sizeof(CoreOutput) == 12, "MMS ARM32 output ABI");
#endif

struct Api {
    void *library;
    int32_t (*create)(void **, const char *, char *, int32_t *);
    int32_t (*destroy)(void *);
    const void *(*config)(void *);
    int32_t (*addGyro)(void *, int64_t, const float *);
    int32_t (*warp)(void *, const CoreFrame *, CoreOutput *);
    Api() : library(nullptr), create(nullptr), destroy(nullptr), config(nullptr),
            addGyro(nullptr), warp(nullptr) {}
    ~Api() { if (library) dlclose(library); }
    // The DSO is opened eagerly, symbols resolved here on first use.
    void open() {
        if (library) return;
        library = dlopen("libmms_gyro_vstab.so", RTLD_NOW | RTLD_LOCAL);
        if (!library) return;
#define LOAD(member, symbol) member = reinterpret_cast<decltype(member)>(dlsym(library, symbol))
        LOAD(create, "vStabCreateUseTextCnfgFile");
        LOAD(destroy, "vStabDestroy");
        LOAD(config, "vStabConfig");
        LOAD(addGyro, "vStabAddAngularVelocity");
        LOAD(warp, "vStabWarpGrid");
#undef LOAD
    }
    bool valid() const { return create && destroy && config && addGyro && warp; }
};

bool isCancelled(const std::atomic<bool> *flag) { return flag && flag->load(); }
using Image = QCamera3GyroVstab::Image;

// Same checks as QCamera3HdrAlgo::validImage, kept local to avoid the HDR link.
bool planeValid(size_t offset, uint32_t stride, uint32_t scanline,
        uint32_t width, uint32_t rows, size_t length) {
    if (stride < width || scanline < rows || !rows) return false;
    const uint64_t end = uint64_t(offset) + uint64_t(stride) * (rows - 1) + width;
    return end >= offset && end <= length;
}
bool validImage(const Image &im) {
    if (!im.data || !im.width || !im.height || (im.width & 1) || (im.height & 1) ||
            im.width > INT32_MAX - 63 || im.height > INT32_MAX ||
            im.yStride > INT32_MAX || im.uvStride > INT32_MAX) return false;
    if (!planeValid(im.yOffset, im.yStride, im.yScanline, im.width, im.height, im.length) ||
            !planeValid(im.uvOffset, im.uvStride, im.uvScanline,
                im.width, im.height / 2, im.length)) return false;
    const uint64_t yEnd = uint64_t(im.yOffset) + uint64_t(im.yStride) * (im.height - 1) + im.width;
    const uint64_t uvEnd = uint64_t(im.uvOffset) + uint64_t(im.uvStride) * (im.height / 2 - 1) + im.width;
    return yEnd <= im.uvOffset || uvEnd <= im.yOffset;
}
void copyImage(const Image &from, const Image &to) {
    for (uint32_t y = 0; y < from.height; ++y)
        memcpy(to.data + to.yOffset + size_t(y) * to.yStride,
                from.data + from.yOffset + size_t(y) * from.yStride, from.width);
    for (uint32_t y = 0; y < from.height / 2; ++y)
        memcpy(to.data + to.uvOffset + size_t(y) * to.uvStride,
                from.data + from.uvOffset + size_t(y) * from.uvStride, from.width);
}
// Bilinear sample: Q24 coordinates, 10-bit weights, edge replication.
uint8_t sampleByte(const uint8_t *base, uint32_t stride, uint32_t width,
        uint32_t height, unsigned step, unsigned channel, int64_t fx, int64_t fy) {
    const int64_t maxX = int64_t(width - 1) * kCoordinateScale;
    const int64_t maxY = int64_t(height - 1) * kCoordinateScale;
    if (fx < 0) fx = 0; else if (fx > maxX) fx = maxX;
    if (fy < 0) fy = 0; else if (fy > maxY) fy = maxY;
    const uint32_t x0 = uint32_t(fx >> kCoordinateBits);
    const uint32_t y0 = uint32_t(fy >> kCoordinateBits);
    const uint32_t x1 = x0 + 1 < width ? x0 + 1 : x0;
    const uint32_t y1 = y0 + 1 < height ? y0 + 1 : y0;
    const uint32_t dx = uint32_t((fx >> (kCoordinateBits - 10)) & 1023);
    const uint32_t dy = uint32_t((fy >> (kCoordinateBits - 10)) & 1023);
    const uint32_t a = base[size_t(y0) * stride + size_t(x0) * step + channel];
    const uint32_t b = base[size_t(y0) * stride + size_t(x1) * step + channel];
    const uint32_t c = base[size_t(y1) * stride + size_t(x0) * step + channel];
    const uint32_t d = base[size_t(y1) * stride + size_t(x1) * step + channel];
    return uint8_t(((a * (1024 - dx) + b * dx) * (1024 - dy) +
            (c * (1024 - dx) + d * dx) * dy + 524288) >> 20);
}
int64_t toFixed(double value) {
    if (!std::isfinite(value)) return 0;
    const double scaled = value * double(kCoordinateScale);
    if (scaled > 9.0e18) return int64_t(9.0e18);
    if (scaled < -9.0e18) return int64_t(-9.0e18);
    return int64_t(std::llround(scaled));
}
bool regularProfile(const char *path) {
    if (!path || !path[0]) return false;
    const int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return false;
    struct stat st;
    const bool ok = !fstat(fd, &st) && S_ISREG(st.st_mode) &&
            st.st_size > 0 && st.st_size <= kMaxProfileBytes;
    close(fd);
    return ok;
}
} // namespace

struct QCamera3GyroVstab::Impl {
    struct Sample { int64_t ts; float w[3]; };
    Sample gyro[kGyroHistory] = {};
    size_t gyroCount = 0, gyroHead = 0;
    int64_t lastGyroTs = 0, lastFrameTs = 0;
    double pan[3] = {}, bias[3] = {};
    bool enabled = false;
    mutable pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

    uint8_t *scratch = nullptr;
    size_t scratchBytes = 0;
    uint32_t scratchWidth = 0, scratchHeight = 0;

    Api api;
    void *engine = nullptr;
    Vertex *vertices = nullptr;
    uint32_t columns = 0, rows = 0;

    Sample &at(size_t index) { return gyro[(gyroHead + index) % kGyroHistory]; }
    const Sample &at(size_t index) const { return gyro[(gyroHead + index) % kGyroHistory]; }
    void push(const Sample &sample) {
        if (gyroCount == kGyroHistory) {
            gyroHead = (gyroHead + 1) % kGyroHistory;
            --gyroCount;
        }
        gyro[(gyroHead + gyroCount) % kGyroHistory] = sample;
        ++gyroCount;
    }
    void clearGyro() {
        gyroCount = 0; gyroHead = 0; lastGyroTs = 0;
        for (unsigned i = 0; i < 3; ++i) { pan[i] = 0.0; bias[i] = 0.0; }
    }
    void clearEngine() {
        if (engine && api.destroy) api.destroy(engine);
        engine = nullptr;
        free(vertices); vertices = nullptr;
        columns = rows = 0;
    }
    void clearScratch() {
        free(scratch); scratch = nullptr; scratchBytes = 0;
        scratchWidth = scratchHeight = 0;
    }
    void interpolate(size_t lo, size_t hi, int64_t ts, double out[3]) const {
        const int64_t t0 = at(lo).ts, t1 = at(hi).ts;
        const double span = double(t1 - t0);
        const double f = span > 0.0 ? double(ts - t0) / span : 0.0;
        for (unsigned a = 0; a < 3; ++a)
            out[a] = double(at(lo).w[a]) + f * (double(at(hi).w[a]) - double(at(lo).w[a]));
    }
    // Trapezoidal integration over [t0, t1]; both edges interpolate.
    void integrate(int64_t t0, int64_t t1, double theta[3], int64_t &maxGapNs) const {
        for (unsigned a = 0; a < 3; ++a) theta[a] = 0.0;
        maxGapNs = 0;
        size_t anchor = 0;
        for (size_t i = 0; i < gyroCount; ++i) {
            if (at(i).ts <= t0) anchor = i;
            else break;
        }
        double current[3];
        interpolate(anchor, anchor + 1, t0, current);
        int64_t currentTs = t0;
        int64_t previousSampleTs = at(anchor).ts;
        for (size_t i = anchor + 1; i < gyroCount && currentTs < t1; ++i) {
            const int64_t sampleTs = at(i).ts;
            const int64_t gap = sampleTs - previousSampleTs;
            if (gap > maxGapNs) maxGapNs = gap;
            previousSampleTs = sampleTs;
            const int64_t end = sampleTs < t1 ? sampleTs : t1;
            double next[3];
            if (end == sampleTs) {
                for (unsigned a = 0; a < 3; ++a) next[a] = double(at(i).w[a]);
            } else {
                interpolate(i - 1, i, end, next);
            }
            const double dt = double(end - currentTs) * 1e-9;
            if (dt > 0.0)
                for (unsigned a = 0; a < 3; ++a)
                    theta[a] += 0.5 * (current[a] + next[a]) * dt;
            for (unsigned a = 0; a < 3; ++a) current[a] = next[a];
            currentTs = end;
        }
    }
};

QCamera3GyroVstab::QCamera3GyroVstab() : mImpl(new (std::nothrow) Impl) {}
QCamera3GyroVstab::~QCamera3GyroVstab() {
    if (mImpl) {
        mImpl->clearEngine();
        mImpl->clearScratch();
        pthread_mutex_destroy(&mImpl->lock);
    }
    delete mImpl;
}

const char *QCamera3GyroVstab::name() { return "GyroVstab"; }

bool QCamera3GyroVstab::isAvailable() { return true; }

bool QCamera3GyroVstab::stockCoreAvailable() {
    // Probed once per process.
    static const bool available = []() {
        Api api;
        api.open();
        return api.valid();
    }();
    return available;
}

bool QCamera3GyroVstab::supports(uint32_t width, uint32_t height) {
    if (!width || !height || (width & 1) || (height & 1)) return false;
    if (width > kMaxDimension || height > kMaxDimension) return false;
    return uint64_t(width) * height * 3 / 2 <= kMaxScratchBytes;
}

bool QCamera3GyroVstab::validOptions(const Options &o) {
    if (o.strength < 0 || o.strength > kMaxStrength) return false;
    if (o.maxCropPercent < 0 || o.maxCropPercent > kMaxCropPercent) return false;
    if (o.windowNs < kMinWindowNs || o.windowNs > kMaxWindowNs) return false;
    if (o.timestampNs <= 0) return false;
    if (o.path != PATH_AUTO && o.path != PATH_NATIVE && o.path != PATH_STOCK) return false;
    if (o.passthrough != 0 && o.passthrough != 1) return false;
    if (o.exposureNs < 0 || o.exposureNs > 1000000000LL) return false;
    if (!std::isfinite(o.zoom) || o.zoom < 1.0f || o.zoom > 16.0f) return false;
    if (o.strength > 0) {
        if (!std::isfinite(o.focalLengthPx) || o.focalLengthPx < kMinFocalPx ||
                o.focalLengthPx > kMaxFocalPx) return false;
    }
    return true;
}

int32_t QCamera3GyroVstab::configureStock(const char *profilePath,
        uint32_t width, uint32_t height) {
    if (!mImpl) return -ENOMEM;
    if (!regularProfile(profilePath) || !supports(width, height)) return -EINVAL;
    pthread_mutex_lock(&mImpl->lock);
    mImpl->clearEngine();
    mImpl->api.open();
    if (!mImpl->api.valid()) {
        pthread_mutex_unlock(&mImpl->lock);
        return -ENODEV;
    }
    char error[256] = {};
    int32_t capacity = sizeof(error);
    int32_t status = mImpl->api.create(&mImpl->engine, profilePath, error, &capacity);
    if (status || !mImpl->engine) {
        mImpl->clearEngine();
        pthread_mutex_unlock(&mImpl->lock);
        return -EIO;
    }
    // Only the grid columns/rows are consumed from the opaque config blob.
    const uint8_t *blob = static_cast<const uint8_t *>(mImpl->api.config(mImpl->engine));
    uint32_t columns = 0, rows = 0;
    if (blob) {
        memcpy(&columns, blob + 0x1c, sizeof(columns));
        memcpy(&rows, blob + 0x20, sizeof(rows));
    }
    if (columns < 2 || columns > kMaxGrid || rows < 2 || rows > kMaxGrid) {
        mImpl->clearEngine();
        pthread_mutex_unlock(&mImpl->lock);
        return -EPROTO;
    }
    mImpl->vertices = static_cast<Vertex *>(calloc(size_t(columns) * rows, sizeof(Vertex)));
    if (!mImpl->vertices) {
        mImpl->clearEngine();
        pthread_mutex_unlock(&mImpl->lock);
        return -ENOMEM;
    }
    mImpl->columns = columns;
    mImpl->rows = rows;
    pthread_mutex_unlock(&mImpl->lock);
    return 0;
}

void QCamera3GyroVstab::setEnabled(bool on) {
    if (!mImpl) return;
    pthread_mutex_lock(&mImpl->lock);
    mImpl->enabled = on;
    pthread_mutex_unlock(&mImpl->lock);
}

bool QCamera3GyroVstab::enabled() const {
    if (!mImpl) return false;
    pthread_mutex_lock(&mImpl->lock);
    const bool on = mImpl->enabled;
    pthread_mutex_unlock(&mImpl->lock);
    return on;
}

int32_t QCamera3GyroVstab::addGyro(int64_t timestampNs, const float angularVelocity[3]) {
    if (!mImpl) return -ENOMEM;
    if (!angularVelocity || timestampNs <= 0) return -EINVAL;
    for (unsigned i = 0; i < 3; ++i)
        if (!std::isfinite(angularVelocity[i])) return -EINVAL;
    pthread_mutex_lock(&mImpl->lock);
    if (mImpl->lastGyroTs && timestampNs <= mImpl->lastGyroTs) {
        pthread_mutex_unlock(&mImpl->lock);
        return -EINVAL;
    }
    if (mImpl->lastGyroTs && timestampNs - mImpl->lastGyroTs > kMaxGyroGapNs) {
        // A feed hole invalidates the history.
        const int64_t keep = mImpl->lastGyroTs;
        mImpl->clearGyro();
        mImpl->lastGyroTs = keep;
    }
    Impl::Sample sample = {};
    sample.ts = timestampNs;
    for (unsigned i = 0; i < 3; ++i) sample.w[i] = angularVelocity[i];
    mImpl->push(sample);
    mImpl->lastGyroTs = timestampNs;
    // A core error only disables the stock grid path.
    if (mImpl->engine && mImpl->api.addGyro)
        (void)mImpl->api.addGyro(mImpl->engine, timestampNs, angularVelocity);
    pthread_mutex_unlock(&mImpl->lock);
    return 0;
}

size_t QCamera3GyroVstab::gyroSamples() const {
    if (!mImpl) return 0;
    pthread_mutex_lock(&mImpl->lock);
    const size_t count = mImpl->gyroCount;
    pthread_mutex_unlock(&mImpl->lock);
    return count;
}

void QCamera3GyroVstab::reset() {
    if (!mImpl) return;
    pthread_mutex_lock(&mImpl->lock);
    mImpl->clearGyro();
    mImpl->lastFrameTs = 0;
    mImpl->clearEngine();
    mImpl->clearScratch();
    pthread_mutex_unlock(&mImpl->lock);
}

namespace {
// Affine bilinear warp; scratch first so cancellation leaves the destination untouched.
int32_t warpImage(const Image &source, const Image &destination, uint8_t *scratch,
        double scaleX, double scaleY, double startX, double startY,
        const std::atomic<bool> *cancelled) {
    const uint32_t width = destination.width, height = destination.height;
    uint8_t *scratchY = scratch;
    uint8_t *scratchUV = scratch + size_t(width) * height;
    const int64_t stepX = toFixed(scaleX);
    const int64_t startXFixed = toFixed(0.5 * scaleX - 0.5 + startX);
    const int64_t scaleYFixed = toFixed(scaleY);
    const int64_t startYFixed = toFixed(startY);
    for (uint32_t y = 0; y < height; ++y) {
        if ((y % kCancelRows) == 0 && isCancelled(cancelled)) return -ECANCELED;
        const int64_t fy = int64_t(y) * scaleYFixed + startYFixed;
        uint8_t *rowY = scratchY + size_t(y) * width;
        int64_t fx = startXFixed;
        for (uint32_t x = 0; x < width; ++x, fx += stepX)
            rowY[x] = sampleByte(source.data + source.yOffset, source.yStride,
                    source.width, source.height, 1, 0, fx, fy);
        if (!(y & 1)) {
            const int64_t fyUV = fy / 2;
            uint8_t *rowUV = scratchUV + size_t(y / 2) * width;
            int64_t fxUV = fx - int64_t(width) * stepX; // restart of this row
            fxUV = fxUV / 2;
            for (uint32_t x = 0; x < width; x += 2, fxUV += stepX)
                for (unsigned c = 0; c < 2; ++c)
                    rowUV[x + c] = sampleByte(source.data + source.uvOffset, source.uvStride,
                            source.width / 2, source.height / 2, 2, c, fxUV, fyUV);
        }
    }
    if (isCancelled(cancelled)) return -ECANCELED;
    for (uint32_t y = 0; y < height; ++y)
        memcpy(destination.data + destination.yOffset + size_t(y) * destination.yStride,
                scratchY + size_t(y) * width, width);
    for (uint32_t y = 0; y < height / 2; ++y)
        memcpy(destination.data + destination.uvOffset + size_t(y) * destination.uvStride,
                scratchUV + size_t(y) * width, width);
    return 0;
}
} // namespace

int32_t QCamera3GyroVstab::process(const Image &source, const Image &destination,
        const Options &options, Result &result, const std::atomic<bool> *cancelled) {
    result = {};
    result.frameNumber = options.frameNumber;
    if (!mImpl) return -ENOMEM;
    if (!validImage(source) || !validImage(destination) ||
            source.width != destination.width || source.height != destination.height ||
            !supports(source.width, source.height) || !validOptions(options)) return -EINVAL;
    if (isCancelled(cancelled)) return -ECANCELED;

    pthread_mutex_lock(&mImpl->lock);
    const bool bypass = !mImpl->enabled || options.strength == 0;
    const int32_t mode = options.path == PATH_AUTO
            ? (mImpl->engine ? PATH_STOCK : PATH_NATIVE) : options.path;

    // Explicit OFF: byte-identical pass-through, no crop and no shift.
    if (bypass) {
        result.reason = mImpl->enabled ? REASON_BYPASS : REASON_DISABLED;
        result.path = PATH_NONE;
        result.cropFactorPermille = 1000;
        copyImage(source, destination);
        pthread_mutex_unlock(&mImpl->lock);
        return 0;
    }
    if (mode == PATH_STOCK && !mImpl->engine) {
        result.reason = REASON_CORE_UNVERIFIED;
        pthread_mutex_unlock(&mImpl->lock);
        return -ENOSYS;
    }
    if (mImpl->lastFrameTs && options.timestampNs <= mImpl->lastFrameTs) {
        result.reason = REASON_FRAME_BACKWARDS;
        pthread_mutex_unlock(&mImpl->lock);
        if (options.passthrough) copyImage(source, destination);
        return -EINVAL;
    }

    const int64_t windowEnd = options.timestampNs;
    const int64_t windowStart = windowEnd - options.windowNs;
    int32_t refusal = 0;
    if (!mImpl->gyroCount) {
        result.reason = REASON_NO_GYRO;
        refusal = -ENODATA;
    } else if (mImpl->at(mImpl->gyroCount - 1).ts < windowEnd) {
        result.reason = REASON_GYRO_STALE;
        refusal = -ETIMEDOUT;
    } else if (mImpl->at(0).ts > windowStart) {
        result.reason = REASON_OUT_OF_WINDOW;
        refusal = -ERANGE;
    }
    double theta[3] = {}, shake[3] = {};
    int64_t maxGapNs = 0;
    if (!refusal) {
        mImpl->integrate(windowStart, windowEnd, theta, maxGapNs);
        if (maxGapNs > kMaxInWindowGapNs) {
            result.reason = REASON_GYRO_STALE;
            refusal = -ETIMEDOUT;
        }
    }
    if (refusal) {
        pthread_mutex_unlock(&mImpl->lock);
        if (options.passthrough) copyImage(source, destination);
        return refusal;
    }

    // A long frame gap restarts the estimator.
    if (mImpl->lastFrameTs && windowEnd - mImpl->lastFrameTs > kMaxFrameGapNs) {
        for (unsigned a = 0; a < 3; ++a) { mImpl->pan[a] = 0.0; mImpl->bias[a] = 0.0; }
    }
    const double windowSec = double(options.windowNs) * 1e-9;
    for (unsigned a = 0; a < 3; ++a) {
        const double rate = theta[a] / windowSec;
        mImpl->bias[a] += kBiasAlpha * (rate - mImpl->bias[a]);
        if (mImpl->bias[a] > kMaxBiasRadPerSec) mImpl->bias[a] = kMaxBiasRadPerSec;
        if (mImpl->bias[a] < -kMaxBiasRadPerSec) mImpl->bias[a] = -kMaxBiasRadPerSec;
        mImpl->pan[a] += kPanAlpha * (theta[a] - mImpl->pan[a]);
        if (mImpl->pan[a] > kMaxPanRad) mImpl->pan[a] = kMaxPanRad;
        if (mImpl->pan[a] < -kMaxPanRad) mImpl->pan[a] = -kMaxPanRad;
        shake[a] = theta[a] - mImpl->pan[a];
        result.biasMilliRadPerSec[a] = int32_t(std::lround(mImpl->bias[a] * 1000.0));
    }

    const double marginFraction = double(options.maxCropPercent) / 200.0;
    const double retain = 1.0 - 2.0 * marginFraction;
    const double scaleToSourceX = double(source.width) / double(destination.width);
    const double scaleToSourceY = double(source.height) / double(destination.height);
    const double strength = double(options.strength) / 100.0;
    const double marginX = marginFraction * double(source.width);
    const double marginY = marginFraction * double(source.height);
    double shiftX = 0.0, shiftY = 0.0;
    double scaleX = retain * scaleToSourceX;
    double scaleY = retain * scaleToSourceY;
    double startX = marginX;
    double startY = marginY;
    int32_t cropPermille = int32_t(std::lround(1000.0 * retain));
    int32_t path = PATH_NATIVE;

    if (mode == PATH_STOCK) {
        CoreFrame input = {uint32_t(sizeof(CoreFrame)), 2, options.timestampNs,
                options.exposureNs, options.zoom, 1.0f, 0, nullptr};
        CoreOutput output = {uint32_t(sizeof(CoreOutput)), 1, mImpl->vertices};
        const int32_t core = mImpl->api.warp(mImpl->engine, &input, &output);
        result.coreStatus = core;
        if (core == 0 && output.size == uint32_t(sizeof(CoreOutput)) && output.revision == 1 &&
                output.vertices == mImpl->vertices) {
            float minX = mImpl->vertices[0].x, maxX = minX;
            float minY = mImpl->vertices[0].y, maxY = minY;
            for (size_t i = 1; i < size_t(mImpl->columns) * mImpl->rows; ++i) {
                const Vertex &v = mImpl->vertices[i];
                if (v.x < minX) minX = v.x; else if (v.x > maxX) maxX = v.x;
                if (v.y < minY) minY = v.y; else if (v.y > maxY) maxY = v.y;
            }
            const double spanX = double(maxX) - double(minX);
            const double spanY = double(maxY) - double(minY);
            if (std::isfinite(spanX) && std::isfinite(spanY) &&
                    spanX > 0.05 && spanY > 0.05 && spanX < 64.0 && spanY < 64.0) {
                // The stock mesh maps NDC [minX, maxX] to source UV [0, 1]:
                // scale = 2 * src / (dst * span), start = -(1 + minX) * src / span.
                const double stockScaleX = 2.0 * double(source.width) /
                        (double(destination.width) * spanX);
                const double stockScaleY = 2.0 * double(source.height) /
                        (double(destination.height) * spanY);
                const double stockStartX = -(1.0 + double(minX)) * double(source.width) / spanX;
                const double stockStartY = -(1.0 + double(minY)) * double(source.height) / spanY;
                scaleX = stockScaleX;
                scaleY = stockScaleY;
                startX = stockStartX;
                startY = stockStartY;
                // Shift of the sampled window centre from the identity mapping, in source pixels.
                shiftX = 0.5 * (double(destination.width) * stockScaleX -
                        double(source.width)) + stockStartX;
                shiftY = 0.5 * (double(destination.height) * stockScaleY -
                        double(source.height)) + stockStartY;
                cropPermille = int32_t(std::lround(1000.0 * 2.0 /
                        (spanX > spanY ? spanX : spanY)));
                if (cropPermille > 1000) cropPermille = 1000;
                if (cropPermille < 1) cropPermille = 1;
                path = PATH_STOCK;
            }
        }
        // Any core failure: the native integrator warps the frame.
    }
    if (path == PATH_NATIVE) {
        double angleYaw = shake[1] * strength;   // about +Y -> horizontal
        double anglePitch = shake[0] * strength; // about +X -> vertical
        // Impossible rotation is clamped, reported and bounded by the crop budget.
        if (angleYaw > kMaxAngleRad) { angleYaw = kMaxAngleRad; result.clamped = 1; }
        if (angleYaw < -kMaxAngleRad) { angleYaw = -kMaxAngleRad; result.clamped = 1; }
        if (anglePitch > kMaxAngleRad) { anglePitch = kMaxAngleRad; result.clamped = 1; }
        if (anglePitch < -kMaxAngleRad) { anglePitch = -kMaxAngleRad; result.clamped = 1; }
        shiftX = double(options.focalLengthPx) * std::tan(angleYaw) * scaleToSourceX;
        shiftY = double(options.focalLengthPx) * std::tan(anglePitch) * scaleToSourceY;
        if (shiftX > marginX) { shiftX = marginX; result.clamped = 1; }
        if (shiftX < -marginX) { shiftX = -marginX; result.clamped = 1; }
        if (shiftY > marginY) { shiftY = marginY; result.clamped = 1; }
        if (shiftY < -marginY) { shiftY = -marginY; result.clamped = 1; }
        startX = marginX + shiftX;
        startY = marginY + shiftY;
    }

    const size_t needed = size_t(destination.width) * destination.height * 3 / 2;
    if (needed > kMaxScratchBytes) {
        pthread_mutex_unlock(&mImpl->lock);
        return -E2BIG;
    }
    if (mImpl->scratchBytes < needed) {
        uint8_t *grown = static_cast<uint8_t *>(realloc(mImpl->scratch, needed));
        if (!grown) {
            pthread_mutex_unlock(&mImpl->lock);
            return -ENOMEM;
        }
        mImpl->scratch = grown;
        mImpl->scratchBytes = needed;
        mImpl->scratchWidth = destination.width;
        mImpl->scratchHeight = destination.height;
    }

    result.gyroCount = int32_t(mImpl->gyroCount);
    const int32_t status = warpImage(source, destination, mImpl->scratch,
            scaleX, scaleY, startX, startY, cancelled);
    if (status == 0) {
        mImpl->lastFrameTs = windowEnd;
        result.status = 0;
        result.applied = options.strength;
        result.path = path;
        result.reason = REASON_OK;
        result.cropFactorPermille = cropPermille;
        result.offsetXMilli = int32_t(std::lround(shiftX * 1000.0));
        result.offsetYMilli = int32_t(std::lround(shiftY * 1000.0));
        result.detail[0] = result.path;
        result.detail[1] = result.cropFactorPermille;
        result.detail[2] = result.offsetXMilli;
        result.detail[3] = result.offsetYMilli;
    }
    pthread_mutex_unlock(&mImpl->lock);
    return status;
}

} // namespace qcamera
