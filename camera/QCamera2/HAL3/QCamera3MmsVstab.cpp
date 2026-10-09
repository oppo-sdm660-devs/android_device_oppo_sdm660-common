/* SPDX-License-Identifier: BSD-3-Clause */
#define LOG_TAG "VSTAB_MMS"

#include "QCamera3MmsVstab.h"
#include "QCamera3MmsGl.h"

#include <algorithm>
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

#include <log/log.h>

#ifndef EUCLEAN
/* bionic and glibc both define EUCLEAN (117). */
#define EUCLEAN 117
#endif

namespace qcamera {
namespace {
constexpr size_t kMemoryLimit = 128U * 1024U * 1024U;
constexpr uint32_t kMaxGrid = 65;
struct CoreFrame {
    uint32_t size, revision;
    int64_t timestampNs, exposureNs;
    float zoom, gain;
    int32_t rotation;
    void *debugImage;
};
struct CoreOutput { uint32_t size, revision; QCamera3MmsVstab::Vertex *vertices; };
static_assert(sizeof(QCamera3MmsVstab::Vertex) == 12, "MMS vertex ABI");
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
    Api() : library(dlopen("libmms_gyro_vstab.so", RTLD_NOW | RTLD_LOCAL)),
            create(nullptr), destroy(nullptr), config(nullptr), addGyro(nullptr),
            warp(nullptr) {
        if (!library) return;
#define LOAD(member, name) member = reinterpret_cast<decltype(member)>(dlsym(library, name))
        LOAD(create, "vStabCreateUseTextCnfgFile");
        LOAD(destroy, "vStabDestroy");
        LOAD(config, "vStabConfig");
        LOAD(addGyro, "vStabAddAngularVelocity");
        LOAD(warp, "vStabWarpGrid");
#undef LOAD
    }
    ~Api() { if (library) dlclose(library); }
    bool valid() const { return create && destroy && config && addGyro && warp; }
};
struct Lock {
    pthread_mutex_t &mutex;
    explicit Lock(pthread_mutex_t &m) : mutex(m) { pthread_mutex_lock(&mutex); }
    ~Lock() { pthread_mutex_unlock(&mutex); }
};
bool isCancelled(const std::atomic<bool> *flag) { return flag && flag->load(); }
using Image = QCamera3MmsVstab::Image;
using Vertex = QCamera3MmsVstab::Vertex;
bool validImage(const Image &image) {
    return image.width <= 8192 && image.height <= 8192 &&
            QCamera3HdrAlgo::validImage(image);
}
Image packed(uint8_t *data, uint32_t width, uint32_t height) {
    const size_t pixels = static_cast<size_t>(width) * height;
    return {data, pixels + pixels / 2, width, height, 0, pixels,
            width, width, height, height / 2};
}
void copyImage(const Image &from, const Image &to) {
    for (uint32_t y = 0; y < from.height; ++y)
        memcpy(to.data + to.yOffset + y * to.yStride,
                from.data + from.yOffset + y * from.yStride, from.width);
    for (uint32_t y = 0; y < from.height / 2; ++y)
        memcpy(to.data + to.uvOffset + y * to.uvStride,
                from.data + from.uvOffset + y * from.uvStride, from.width);
}

/* Consecutive core failures before the session latches into pass-through. */
constexpr uint32_t kCoreFailureLatch = 8;

/* 10-bit bilinear weights over a 16.16 fixed-point source coordinate. */
constexpr int32_t kPassFracBits = 16;
constexpr int64_t kPassOne = int64_t(1) << kPassFracBits;
constexpr int32_t kPassWeightBits = 10;
constexpr uint32_t kPassWeightOne = 1U << kPassWeightBits;

inline uint32_t passBilinear(const uint8_t *row0, const uint8_t *row1,
        uint32_t x0, uint32_t x1, uint32_t wx, uint32_t wy) {
    const uint32_t top = uint32_t(row0[x0]) * (kPassWeightOne - wx) +
            uint32_t(row0[x1]) * wx;
    const uint32_t bottom = uint32_t(row1[x0]) * (kPassWeightOne - wx) +
            uint32_t(row1[x1]) * wx;
    return (top * (kPassWeightOne - wy) + bottom * wy +
            kPassWeightOne * kPassWeightOne / 2) >> (2 * kPassWeightBits);
}

// Full-frame integer bilinear scale; no floating point and no allocation.
int32_t passThroughCpu(const Image &source, const Image &destination,
        const std::atomic<bool> *cancelled) {
    const uint32_t srcW = source.width, srcH = source.height;
    const uint32_t dstW = destination.width, dstH = destination.height;
    const int64_t stepX = (int64_t(srcW) << kPassFracBits) / dstW;
    const int64_t stepY = (int64_t(srcH) << kPassFracBits) / dstH;
    if (stepX <= 0 || stepY <= 0) return -EINVAL;
    const int64_t maxFx = int64_t(srcW - 1) << kPassFracBits;
    const int64_t maxFy = int64_t(srcH - 1) << kPassFracBits;
    const int64_t startX = stepX / 2 - kPassOne / 2;
    const int64_t startY = stepY / 2 - kPassOne / 2;
    for (uint32_t y = 0; y < dstH; ++y) {
        if ((y & 63U) == 0 && isCancelled(cancelled)) return -ECANCELED;
        int64_t fy = startY + int64_t(y) * stepY;
        if (fy < 0) fy = 0;
        else if (fy > maxFy) fy = maxFy;
        const uint32_t y0 = uint32_t(fy >> kPassFracBits);
        const uint32_t y1 = std::min<uint32_t>(y0 + 1, srcH - 1);
        const uint32_t wy = uint32_t((fy >> (kPassFracBits - kPassWeightBits)) &
                (kPassWeightOne - 1));
        const uint8_t *row0 = source.data + source.yOffset + size_t(y0) * source.yStride;
        const uint8_t *row1 = source.data + source.yOffset + size_t(y1) * source.yStride;
        uint8_t *dstRow = destination.data + destination.yOffset +
                size_t(y) * destination.yStride;
        int64_t fx = startX;
        for (uint32_t x = 0; x < dstW; ++x, fx += stepX) {
            int64_t cx = fx;
            if (cx < 0) cx = 0;
            else if (cx > maxFx) cx = maxFx;
            const uint32_t x0 = uint32_t(cx >> kPassFracBits);
            const uint32_t x1 = std::min<uint32_t>(x0 + 1, srcW - 1);
            const uint32_t wx = uint32_t((cx >> (kPassFracBits - kPassWeightBits)) &
                    (kPassWeightOne - 1));
            dstRow[x] = uint8_t(passBilinear(row0, row1, x0, x1, wx, wy));
        }
    }
    // Both planes stay interleaved (NV12/NV21).
    const uint32_t srcUVW = srcW / 2, srcUVH = srcH / 2;
    const uint32_t dstUVW = dstW / 2, dstUVH = dstH / 2;
    const int64_t stepUVX = (int64_t(srcUVW) << kPassFracBits) / dstUVW;
    const int64_t stepUVY = (int64_t(srcUVH) << kPassFracBits) / dstUVH;
    if (stepUVX <= 0 || stepUVY <= 0) return -EINVAL;
    const int64_t maxUVX = int64_t(srcUVW - 1) << kPassFracBits;
    const int64_t maxUVY = int64_t(srcUVH - 1) << kPassFracBits;
    const int64_t startUVX = stepUVX / 2 - kPassOne / 2;
    const int64_t startUVY = stepUVY / 2 - kPassOne / 2;
    for (uint32_t y = 0; y < dstUVH; ++y) {
        if ((y & 31U) == 0 && isCancelled(cancelled)) return -ECANCELED;
        int64_t fy = startUVY + int64_t(y) * stepUVY;
        if (fy < 0) fy = 0;
        else if (fy > maxUVY) fy = maxUVY;
        const uint32_t y0 = uint32_t(fy >> kPassFracBits);
        const uint32_t y1 = std::min<uint32_t>(y0 + 1, srcUVH - 1);
        const uint32_t wy = uint32_t((fy >> (kPassFracBits - kPassWeightBits)) &
                (kPassWeightOne - 1));
        const uint8_t *row0 = source.data + source.uvOffset + size_t(y0) * source.uvStride;
        const uint8_t *row1 = source.data + source.uvOffset + size_t(y1) * source.uvStride;
        uint8_t *dstRow = destination.data + destination.uvOffset +
                size_t(y) * destination.uvStride;
        int64_t fx = startUVX;
        for (uint32_t x = 0; x < dstUVW; ++x, fx += stepUVX) {
            int64_t cx = fx;
            if (cx < 0) cx = 0;
            else if (cx > maxUVX) cx = maxUVX;
            const uint32_t x0 = uint32_t(cx >> kPassFracBits);
            const uint32_t x1 = std::min<uint32_t>(x0 + 1, srcUVW - 1);
            const uint32_t wx = uint32_t((cx >> (kPassFracBits - kPassWeightBits)) &
                    (kPassWeightOne - 1));
            const uint32_t offset0 = x0 * 2, offset1 = x1 * 2;
            dstRow[x * 2] = uint8_t(passBilinear(row0, row1, offset0, offset1, wx, wy));
            dstRow[x * 2 + 1] = uint8_t(passBilinear(row0 + 1, row1 + 1,
                    offset0, offset1, wx, wy));
        }
    }
    return 0;
}

/* Full-frame identity mesh for the GPU pass-through path. */
constexpr Vertex kIdentityMesh[4] = {
    {-1.0f, -1.0f, 0.0f}, {1.0f, -1.0f, 0.0f},
    {-1.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 0.0f},
};
// Parses only the boundary fields; the core parses the full profile.
struct Profile { int64_t value[8]; };
int32_t readProfile(const char *path, Profile &profile) {
    static const char *keys[] = {"frmWidth", "frmHeight", "gridVrtxCntH",
        "gridVrtxCntV", "isGridNormalized", "gyroTmrInx", "frmTmrInx",
        "averageGyroTime"};
    if (!path || !path[0]) return -EINVAL;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return -errno;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > 65536) {
        close(fd);
        return -EINVAL;
    }
    FILE *file = fdopen(fd, "r");
    if (!file) { int error = errno; close(fd); return -error; }
    char line[1024];
    unsigned found = 0;
    int32_t status = 0;
    while (fgets(line, sizeof(line), file)) {
        if (!strchr(line, '\n') && !feof(file)) { status = -EINVAL; break; }
        char *comment = strchr(line, '#');
        if (comment) *comment = '\0';
        comment = strstr(line, "//");
        if (comment) *comment = '\0';
        char key[128], tail;
        long long value;
        char *entry = line;
        while (*entry == ' ' || *entry == '\t') ++entry;
        char *equals = strchr(entry, '=');
        if (!equals) continue;
        char *endKey = equals;
        while (endKey > entry && (endKey[-1] == ' ' || endKey[-1] == '\t')) --endKey;
        size_t keyLength = static_cast<size_t>(endKey - entry);
        if (!keyLength || keyLength >= sizeof(key)) continue;
        memcpy(key, entry, keyLength); key[keyLength] = '\0';
        for (unsigned i = 0; i < 8; ++i) {
            if (strcmp(key, keys[i])) continue;
            if ((found & (1U << i)) || sscanf(equals + 1, " %lld %c", &value, &tail) != 1) {
                status = -EINVAL; break;
            }
            profile.value[i] = value; found |= 1U << i;
        }
        if (status) break;
    }
    if (ferror(file)) status = -EIO;
    fclose(file);
    return status ? status : found == 255 ? 0 : -EINVAL;
}
double cross(double ax, double ay, double bx, double by) { return ax * by - ay * bx; }
// Q24 coordinates and 10-bit bilinear weights keep the error within one level.
constexpr int kCoordinateBits = 24;
constexpr int64_t kCoordinateScale = int64_t(1) << kCoordinateBits;
uint8_t sample(const Image &source, bool chroma, unsigned channel, int64_t fx, int64_t fy) {
    const uint32_t width = source.width / (chroma ? 2 : 1);
    const uint32_t height = source.height / (chroma ? 2 : 1);
    const unsigned step = chroma ? 2 : 1;
    fx = std::max(int64_t(0), std::min(int64_t(width - 1) * kCoordinateScale, fx));
    fy = std::max(int64_t(0), std::min(int64_t(height - 1) * kCoordinateScale, fy));
    uint32_t x0 = uint32_t(fx >> kCoordinateBits), y0 = uint32_t(fy >> kCoordinateBits);
    uint32_t x1 = std::min(x0 + 1, width - 1), y1 = std::min(y0 + 1, height - 1);
    uint32_t dx = uint32_t((fx >> (kCoordinateBits - 10)) & 1023);
    uint32_t dy = uint32_t((fy >> (kCoordinateBits - 10)) & 1023);
    const uint8_t *base = source.data + (chroma ? source.uvOffset : source.yOffset);
    uint32_t stride = chroma ? source.uvStride : source.yStride;
    uint32_t a = base[y0 * stride + x0 * step + channel];
    uint32_t b = base[y0 * stride + x1 * step + channel];
    uint32_t c = base[y1 * stride + x0 * step + channel];
    uint32_t d = base[y1 * stride + x1 * step + channel];
    return uint8_t(((a * (1024 - dx) + b * dx) * (1024 - dy) +
            (c * (1024 - dx) + d * dx) * dy + 524288) >> 20);
}
// Row zero of NDC and of the source both refer to the first buffer row (no flip).
int32_t triangle(const Image &source, const Image &destination, uint8_t *coverage,
        const Vertex *vertices, uint32_t columns, uint32_t rows,
        unsigned i0, unsigned i1, unsigned i2, bool chroma) {
    const unsigned indices[] = {i0, i1, i2};
    double x[3], y[3], u[3], v[3];
    uint32_t width = destination.width / (chroma ? 2 : 1);
    uint32_t height = destination.height / (chroma ? 2 : 1);
    for (unsigned i = 0; i < 3; ++i) {
        x[i] = (vertices[indices[i]].x + 1.0) * width / 2.0;
        y[i] = (vertices[indices[i]].y + 1.0) * height / 2.0;
        u[i] = static_cast<double>(indices[i] % columns) / (columns - 1);
        v[i] = static_cast<double>(indices[i] / columns) / (rows - 1);
    }
    double area = cross(x[1] - x[0], y[1] - y[0], x[2] - x[0], y[2] - y[0]);
    if (area <= 1e-10) return -ERANGE;
    int left = std::max(0, static_cast<int>(std::ceil(std::min({x[0], x[1], x[2]}) - 0.5)));
    int right = std::min(static_cast<int>(width - 1),
            static_cast<int>(std::floor(std::max({x[0], x[1], x[2]}) - 0.5)));
    int top = std::max(0, static_cast<int>(std::ceil(std::min({y[0], y[1], y[2]}) - 0.5)));
    int bottom = std::min(static_cast<int>(height - 1),
            static_cast<int>(std::floor(std::max({y[0], y[1], y[2]}) - 0.5)));
    uint8_t *base = destination.data + (chroma ? destination.uvOffset : destination.yOffset);
    uint32_t stride = chroma ? destination.uvStride : destination.yStride;
    if (left > right || top > bottom) return 0;
    const double db = (y[2] - y[0]) / area, dc = -(y[1] - y[0]) / area;
    const uint32_t sourceWidth = source.width / (chroma ? 2 : 1);
    const uint32_t sourceHeight = source.height / (chroma ? 2 : 1);
    const double sxStep = ((u[1] - u[0]) * db + (u[2] - u[0]) * dc) * sourceWidth;
    const double syStep = ((v[1] - v[0]) * db + (v[2] - v[0]) * dc) * sourceHeight;
    // Reject transforms that could overflow the Q24 range.
    constexpr double maxCoordinate = 1000000000.0;
    if (!std::isfinite(sxStep) || !std::isfinite(syStep) ||
            std::abs(sxStep) > maxCoordinate || std::abs(syStep) > maxCoordinate) return -ERANGE;
    const int64_t fxStep = std::llround(sxStep * kCoordinateScale);
    const int64_t fyStep = std::llround(syStep * kCoordinateScale);
    for (int py = top; py <= bottom; ++py) {
        double b = cross(left + 0.5 - x[0], py + 0.5 - y[0], x[2] - x[0], y[2] - y[0]) / area;
        double c = cross(x[1] - x[0], y[1] - y[0], left + 0.5 - x[0], py + 0.5 - y[0]) / area;
        const double sx = (u[0] + (u[1] - u[0]) * b + (u[2] - u[0]) * c) * sourceWidth - 0.5;
        const double sy = (v[0] + (v[1] - v[0]) * b + (v[2] - v[0]) * c) * sourceHeight - 0.5;
        if (!std::isfinite(sx) || !std::isfinite(sy) ||
                std::abs(sx) + std::abs(sxStep) * (right - left) > maxCoordinate ||
                std::abs(sy) + std::abs(syStep) * (right - left) > maxCoordinate) return -ERANGE;
        int64_t fx = std::llround(sx * kCoordinateScale), fy = std::llround(sy * kCoordinateScale);
        for (int px = left; px <= right; ++px, b += db, c += dc, fx += fxStep, fy += fyStep) {
            if (b < -1e-9 || c < -1e-9 || 1.0 - b - c < -1e-9) continue;
            coverage[py * width + px] = 1;
            base[py * stride + px * (chroma ? 2 : 1)] = sample(source, chroma, 0, fx, fy);
            if (chroma) base[py * stride + px * 2 + 1] = sample(source, true, 1, fx, fy);
        }
    }
    return 0;
}
} // namespace

struct QCamera3MmsVstab::Impl {
    struct Pending { Image image; Frame frame; int64_t queuedNs; } frames[8] = {};
    mutable pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    std::atomic<bool> cancelled{false};
    Api api;
    void *engine = nullptr;
    Vertex *vertices = nullptr;
    Configuration config = {};
    uint32_t columns = 0, rows = 0;
    size_t count = 0, bytes = 0;
    int64_t lastGyroNs = 0, lastFrameNs = 0, periodNs = 0;
    uint32_t consecutiveCoreFailures = 0;
    bool coreDegraded = false;
    int32_t lastCoreStatus = 0;
    uint32_t addGyroFailures = 0;
    uint64_t sessionFrameIndex = 0;
    void pop() {
        if (!count) return;
        bytes -= frames[0].image.length;
        free(frames[0].image.data);
        --count;
        memmove(frames, frames + 1, count * sizeof(frames[0]));
        frames[count] = {};
    }
    void clear() {
        while (count) pop();
        if (engine) api.destroy(engine);
        engine = nullptr;
        free(vertices); vertices = nullptr;
        lastGyroNs = lastFrameNs = periodNs = 0;
        consecutiveCoreFailures = 0;
        coreDegraded = false;
        lastCoreStatus = 0;
        addGyroFailures = 0;
        sessionFrameIndex = 0;
    }
};

QCamera3MmsVstab::QCamera3MmsVstab() : mImpl(new (std::nothrow) Impl) {}
QCamera3MmsVstab::~QCamera3MmsVstab() {
    flush();
    if (mImpl) pthread_mutex_destroy(&mImpl->lock);
    delete mImpl;
}
bool QCamera3MmsVstab::isAvailable() {
    if (sizeof(void *) != 4) return false;
    Api api;
    return api.valid();
}
int32_t QCamera3MmsVstab::configure(const Configuration &config) {
    if (!mImpl) return -ENOMEM;
    flush();
    Lock lock(mImpl->lock);
    if (sizeof(void *) != 4 || !mImpl->api.valid()) return -ENODEV;
    if (config.maxPending < 1 || config.maxPending > 8 || config.maxWaitNs <= 0 ||
            config.maxWaitNs > 1000000000LL || config.inputWidth < 2 || config.inputHeight < 2 ||
            config.inputWidth > 8192 || config.inputHeight > 8192 ||
            (config.inputWidth & 1) || (config.inputHeight & 1) ||
            config.outputWidth < 2 || config.outputHeight < 2 ||
            config.outputWidth > config.inputWidth || config.outputHeight > config.inputHeight ||
            (config.outputWidth & 1) || (config.outputHeight & 1) ||
            uint64_t(config.inputWidth) * config.outputHeight !=
            uint64_t(config.outputWidth) * config.inputHeight) return -EINVAL;
    Profile profile = {};
    int32_t status = readProfile(config.profilePath, profile);
    if (status) return status;
    if (profile.value[0] != config.inputWidth || profile.value[1] != config.inputHeight ||
            profile.value[2] < 2 || profile.value[2] > kMaxGrid ||
            profile.value[3] < 2 || profile.value[3] > kMaxGrid || profile.value[4] != 1 ||
            profile.value[5] != 7 || profile.value[6] != 7 ||
            profile.value[7] < 1000000 || profile.value[7] > 20000000) return -EINVAL;
    if (uint64_t(config.inputWidth) * config.inputHeight * 3 / 2 * config.maxPending +
            uint64_t(config.outputWidth) * config.outputHeight * 3 > kMemoryLimit) return -E2BIG;
    char error[1024] = {};
    int32_t capacity = sizeof(error);
    status = mImpl->api.create(&mImpl->engine, config.profilePath, error, &capacity);
    if (status || !mImpl->engine) { mImpl->clear(); return -EIO; }
    const uint8_t *coreConfig = static_cast<const uint8_t *>(mImpl->api.config(mImpl->engine));
    uint32_t columns = 0, rows = 0;
    if (coreConfig) {
        memcpy(&columns, coreConfig + 0x1c, sizeof(columns));
        memcpy(&rows, coreConfig + 0x20, sizeof(rows));
    }
    if (columns != profile.value[2] || rows != profile.value[3]) {
        mImpl->clear(); return -EPROTO;
    }
    mImpl->vertices = static_cast<Vertex *>(calloc(size_t(columns) * rows, sizeof(Vertex)));
    if (!mImpl->vertices) { mImpl->clear(); return -ENOMEM; }
    mImpl->config = config;
    mImpl->config.profilePath = nullptr;
    mImpl->columns = columns; mImpl->rows = rows; mImpl->periodNs = profile.value[7];
    mImpl->cancelled.store(false);
    return 0;
}
int32_t QCamera3MmsVstab::addGyro(int64_t timestampNs, const float velocity[3]) {
    if (!mImpl) return -ENOMEM;
    Lock lock(mImpl->lock);
    if (!mImpl->engine || mImpl->cancelled.load()) return -ENODEV;
    if (!velocity || timestampNs <= mImpl->lastGyroNs) return -EINVAL;
    for (unsigned i = 0; i < 3; ++i) if (!std::isfinite(velocity[i])) return -EINVAL;
    // The engine applies the gyro-to-camera rotation and keeps the sensor event time.
    int32_t status = mImpl->api.addGyro(mImpl->engine, timestampNs, velocity);
    if (status) {
        ++mImpl->addGyroFailures;
        return -EIO;
    }
    mImpl->lastGyroNs = timestampNs;
    return 0;
}
int32_t QCamera3MmsVstab::enqueue(const Image &source, const Frame &frame, int64_t nowNs) {
    if (!mImpl) return -ENOMEM;
    Lock lock(mImpl->lock);
    if (!mImpl->engine || mImpl->cancelled.load()) return -ENODEV;
    if (!validImage(source) || source.width != mImpl->config.inputWidth ||
            source.height != mImpl->config.inputHeight || frame.timestampNs <= mImpl->lastFrameNs ||
            frame.exposureNs <= 0 || frame.exposureNs > 1000000000LL ||
            !std::isfinite(frame.zoom) || frame.zoom < 1.0f || frame.zoom > 16.0f ||
            nowNs < frame.timestampNs || nowNs - frame.timestampNs > 1000000000LL) return -EINVAL;
    if (mImpl->count >= mImpl->config.maxPending) return -ENOSPC;
    size_t length = size_t(source.width) * source.height * 3 / 2;
    uint8_t *data = static_cast<uint8_t *>(malloc(length));
    if (!data) return -ENOMEM;
    Image copy = packed(data, source.width, source.height);
    copyImage(source, copy);
    mImpl->frames[mImpl->count++] = {copy, frame, nowNs};
    mImpl->bytes += length; mImpl->lastFrameNs = frame.timestampNs;
    return 0;
}
int32_t QCamera3MmsVstab::processNext(const Image &destination, int64_t nowNs, Result &result) {
    result = {};
    if (!mImpl) return -ENOMEM;
    Lock lock(mImpl->lock);
    if (!mImpl->engine || mImpl->cancelled.load()) return -ENODEV;
    if (!mImpl->count) return -ENOENT;
    const Impl::Pending &pending = mImpl->frames[0];
    if (!validImage(destination) || destination.width != mImpl->config.outputWidth ||
            destination.height != mImpl->config.outputHeight || nowNs < pending.queuedNs) return -EINVAL;
    result.frame = pending.frame;
    result.sessionFrameIndex = static_cast<uint32_t>(++mImpl->sessionFrameIndex);
    result.gyroDeltaNs = mImpl->lastGyroNs ?
            pending.frame.timestampNs - mImpl->lastGyroNs : INT64_MIN;
    result.addGyroFailures = static_cast<int32_t>(mImpl->addGyroFailures);
    int32_t status;
    int32_t fallback = 0; // non-zero: this frame is delivered un-stabilised
    if (mImpl->coreDegraded) {
        // Latched: skip the core and deliver real (un-stabilised) frames.
        fallback = -EUCLEAN;
        status = renderPassthrough(pending.image, destination, &mImpl->cancelled);
    } else if (nowNs - pending.queuedNs >= mImpl->config.maxWaitNs) {
        // Core still retrying after the wait budget: deliver un-stabilised.
        fallback = -ETIMEDOUT;
        status = renderPassthrough(pending.image, destination, &mImpl->cancelled);
    } else {
        const Frame &frame = pending.frame;
        CoreFrame input = {40, 2, frame.timestampNs, frame.exposureNs, frame.zoom, 1.0f, 0, nullptr};
        CoreOutput output = {12, 1, mImpl->vertices};
        result.coreStatus = mImpl->api.warp(mImpl->engine, &input, &output);
        mImpl->lastCoreStatus = result.coreStatus;
        // Stock retry statuses (5 = insufficient initial samples); every retry is bounded.
        const int32_t core = result.coreStatus;
        if (mImpl->cancelled.load()) status = -ECANCELED;
        else if (core == 2 || core == 4 || core == 5 || core == 27 || core == 20000 || core == 20001)
            return -EAGAIN;
        else if (core) status = -EIO;
        else if (output.size != 12 || output.revision != 1 || output.vertices != mImpl->vertices)
            status = -EPROTO;
        else status = render(pending.image, destination, mImpl->vertices,
                mImpl->columns, mImpl->rows, &mImpl->cancelled);
    }
    result.coreStatus = mImpl->lastCoreStatus;
    if (status != 0 && status != -ECANCELED && fallback == 0) {
        // Core failure: deliver un-stabilised with the real errno preserved.
        const int32_t failure = status;
        const int32_t passed = renderPassthrough(pending.image, destination, &mImpl->cancelled);
        if (passed == 0 || passed == -ECANCELED) {
            status = passed;
            fallback = failure;
        }
    }
    if (fallback != 0 && status == 0) {
        result.fallbackReason = fallback;
        if (!mImpl->coreDegraded &&
                ++mImpl->consecutiveCoreFailures >= kCoreFailureLatch) {
            mImpl->coreDegraded = true;
            ALOGW("MMS core latched into pass-through after %u consecutive failures; "
                  "frames are delivered WITHOUT stabilisation for the rest of this "
                  "session (last coreStatus=%d)",
                    unsigned(kCoreFailureLatch), mImpl->lastCoreStatus);
        }
    } else if (status == 0) {
        mImpl->consecutiveCoreFailures = 0;
    }
    result.coreDegraded = mImpl->coreDegraded;
    mImpl->pop();
    return status;
}
void QCamera3MmsVstab::flush() {
    if (!mImpl) return;
    mImpl->cancelled.store(true);
    Lock lock(mImpl->lock);
    mImpl->clear();
}
int32_t QCamera3MmsVstab::discard(uint32_t frameNumber) {
    if (!mImpl) return -ENOMEM;
    Lock lock(mImpl->lock);
    if (!mImpl->count) return -ENOENT;
    if (mImpl->frames[0].frame.frameNumber != frameNumber) return -EINVAL;
    mImpl->pop();
    return 0;
}
size_t QCamera3MmsVstab::pending() const {
    if (!mImpl) return 0;
    Lock lock(mImpl->lock); return mImpl->count;
}
int64_t QCamera3MmsVstab::gyroPeriodNs() const {
    if (!mImpl) return 0;
    Lock lock(mImpl->lock); return mImpl->periodNs;
}
bool QCamera3MmsVstab::coreDegraded() const {
    if (!mImpl) return false;
    Lock lock(mImpl->lock); return mImpl->coreDegraded;
}
int32_t QCamera3MmsVstab::lastCoreStatus() const {
    if (!mImpl) return 0;
    Lock lock(mImpl->lock); return mImpl->lastCoreStatus;
}
int32_t QCamera3MmsVstab::addGyroFailures() const {
    if (!mImpl) return 0;
    Lock lock(mImpl->lock);
    return static_cast<int32_t>(mImpl->addGyroFailures);
}
int32_t QCamera3MmsVstab::renderPassthrough(const Image &source,
        const Image &destination, const std::atomic<bool> *cancelled) {
    if (!validImage(source) || !validImage(destination) ||
            destination.width > source.width || destination.height > source.height)
        return -EINVAL;
    if (isCancelled(cancelled)) return -ECANCELED;
    if (QCamera3MmsGl::enabled()) {
        // GPU identity draw when selected; any refusal falls through to the CPU renderer.
        const int32_t gpu = QCamera3MmsGl::renderIfEnabled(
                source, destination, kIdentityMesh, 2, 2, cancelled);
        if (gpu == 0) return 0;
        if (gpu == -ECANCELED) return -ECANCELED;
    }
    return passThroughCpu(source, destination, cancelled);
}

int32_t QCamera3MmsVstab::render(const Image &source, const Image &destination,
        const Vertex *vertices, uint32_t columns, uint32_t rows,
        const std::atomic<bool> *cancelled) {
    // GPU dispatch: persist.vendor.camera.oppo.vstab.gpu, default 1.
    if (QCamera3MmsGl::enabled()) {
        const int32_t gpu = QCamera3MmsGl::renderIfEnabled(
                source, destination, vertices, columns, rows, cancelled);
        if (gpu == 0) return 0;
        // A cancelled frame is not re-rendered on the CPU.
        if (gpu == -ECANCELED) return -ECANCELED;
    }
    if (!validImage(source) || !validImage(destination) || !vertices ||
            columns < 2 || rows < 2 || columns > kMaxGrid || rows > kMaxGrid) return -EINVAL;
    if (isCancelled(cancelled)) return -ECANCELED;
    for (size_t i = 0; i < size_t(columns) * rows; ++i) {
        if (!std::isfinite(vertices[i].x) || !std::isfinite(vertices[i].y) ||
                !std::isfinite(vertices[i].z) || std::abs(vertices[i].x) > 16.0f ||
                std::abs(vertices[i].y) > 16.0f || std::abs(vertices[i].z) > 1.0f) return -ERANGE;
    }
    size_t pixels = size_t(destination.width) * destination.height;
    if (pixels * 5 / 2 > kMemoryLimit) return -E2BIG;
    uint8_t *storage = static_cast<uint8_t *>(malloc(pixels * 5 / 2));
    if (!storage) return -ENOMEM;
    Image scratch = packed(storage, destination.width, destination.height);
    uint8_t *coverage = storage + scratch.length;
    int32_t status = 0;
    for (unsigned plane = 0; plane < 2 && !status; ++plane) {
        size_t coveredSize = pixels / (plane ? 4 : 1);
        memset(coverage, 0, coveredSize);
        for (uint32_t r = 0; r + 1 < rows && !status; ++r) {
            if (isCancelled(cancelled)) { status = -ECANCELED; break; }
            for (uint32_t c = 0; c + 1 < columns && !status; ++c) {
                unsigned a = r * columns + c, b = a + 1, d = a + columns, e = d + 1;
                if (!(r & 1)) {
                    status = triangle(source, scratch, coverage, vertices, columns, rows, a, b, d, plane);
                    if (!status) status = triangle(source, scratch, coverage, vertices, columns, rows, b, e, d, plane);
                } else {
                    status = triangle(source, scratch, coverage, vertices, columns, rows, a, b, e, plane);
                    if (!status) status = triangle(source, scratch, coverage, vertices, columns, rows, a, e, d, plane);
                }
            }
        }
        if (!status && memchr(coverage, 0, coveredSize)) status = -ERANGE;
    }
    if (!status && isCancelled(cancelled)) status = -ECANCELED;
    if (!status) copyImage(scratch, destination);
    free(storage);
    return status;
}
} // namespace qcamera
