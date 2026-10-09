/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3DualBokehAlgo.h"

#include <cmath>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

namespace qcamera {
namespace {
struct ArcImage {
    uint32_t format;
    int32_t width, height;
    uint8_t *planes[4];
    int32_t pitches[4];
};
struct CameraImageInfo { int32_t wideWidth, wideHeight, teleWidth, teleHeight; };
struct Calibration { uint8_t *data; int32_t length; };
struct DisparityParam {
    int32_t degree;
    float maxFov;
    void *faceRects;
    void *faceOrientations;
    int32_t faceCount;
};
struct RefocusParam {
    int32_t x, y, blurLevel, useRect;
    int32_t left, top, right, bottom;
};
#if UINTPTR_MAX == UINT32_MAX
static_assert(sizeof(ArcImage) == 44, "ARC_DCIR ARM32 image ABI");
static_assert(sizeof(Calibration) == 8, "ARC_DCIR ARM32 calibration ABI");
static_assert(sizeof(DisparityParam) == 20, "ARC_DCIR ARM32 disparity ABI");
#endif
static_assert(sizeof(CameraImageInfo) == 16, "ARC_DCIR camera geometry ABI");
static_assert(sizeof(RefocusParam) == 32, "ARC_DCIR refocus ABI");

pthread_mutex_t gDualBokehLock = PTHREAD_MUTEX_INITIALIZER;
struct Lock {
    Lock() { pthread_mutex_lock(&gDualBokehLock); }
    ~Lock() { pthread_mutex_unlock(&gDualBokehLock); }
};
struct Api {
    void *library;
    int32_t (*init)(void **, int32_t);
    int32_t (*uninit)(void **);
    int32_t (*setCalibration)(void *, Calibration *);
    int32_t (*setGeometry)(void *, CameraImageInfo *);
    int32_t (*reset)(void *);
    int32_t (*calculate)(void *, ArcImage *, ArcImage *, DisparityParam *);
    int32_t (*depthSize)(void *, int32_t *);
    int32_t (*depthData)(void *, uint8_t *);
    int32_t (*process)(void *, uint8_t *, int32_t, ArcImage *, RefocusParam *, ArcImage *);
    Api() : library(dlopen("libarcsoft_dualcam_refocus.so", RTLD_NOW | RTLD_LOCAL)),
            init(nullptr), uninit(nullptr), setCalibration(nullptr), setGeometry(nullptr),
            reset(nullptr), calculate(nullptr), depthSize(nullptr), depthData(nullptr),
            process(nullptr) {
        if (!library) return;
#define LOAD(member, name) member = reinterpret_cast<decltype(member)>(dlsym(library, name))
        LOAD(init, "ARC_DCIR_Init");
        LOAD(uninit, "ARC_DCIR_Uninit");
        LOAD(setCalibration, "ARC_DCIR_SetCaliData");
        LOAD(setGeometry, "ARC_DCIR_SetCameraImageInfo");
        LOAD(reset, "ARC_DCIR_Reset");
        LOAD(calculate, "ARC_DCIR_CalcDisparityData");
        LOAD(depthSize, "ARC_DCIR_GetDisparityDataSize");
        LOAD(depthData, "ARC_DCIR_GetDisparityData");
        LOAD(process, "ARC_DCIR_Process");
#undef LOAD
        // Stock builds the exact disparity record itself. Its unused public
        // GetDefaultParam has an internal stack-size mismatch; do not call it.
    }
    ~Api() { if (library) dlclose(library); }
    bool valid() const {
        return init && uninit && setCalibration && setGeometry && reset &&
                calculate && depthSize && depthData && process;
    }
};
constexpr size_t kCalibrationSize = 512;
constexpr uint64_t kScratchLimit = 128U * 1024U * 1024U;
constexpr int32_t kDepthLimit = 64U * 1024U * 1024U;
bool cancelled(const std::atomic<bool> *flag) { return flag && flag->load(); }
float readFloat(const uint8_t *data, size_t offset) {
    float value;
    memcpy(&value, data + offset, sizeof(value));
    return value;
}
int16_t readShort(const uint8_t *data, size_t offset) {
    int16_t value;
    memcpy(&value, data + offset, sizeof(value));
    return value;
}
int32_t readFile(const char *path, uint8_t *data, size_t length, bool *opened = nullptr) {
    if (opened) *opened = false;
    if (!path) return -EINVAL;
    const int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return -errno;
    if (opened) *opened = true;
    struct stat info = {};
    int32_t rc = 0;
    if (fstat(fd, &info) != 0) rc = -errno;
    else if (!S_ISREG(info.st_mode)) rc = -EINVAL;
    size_t offset = 0;
    while (rc == 0 && offset < length) {
        const ssize_t count = read(fd, data + offset, length - offset);
        if (count > 0) offset += size_t(count);
        else if (count == 0) rc = -EMSGSIZE;
        else if (errno != EINTR) rc = -errno;
    }
    close(fd);
    return rc;
}
void copyPlane(uint8_t *destination, uint32_t destinationStride,
        const uint8_t *source, uint32_t sourceStride, uint32_t width, uint32_t rows) {
    for (uint32_t row = 0; row < rows; ++row)
        memcpy(destination + size_t(row) * destinationStride,
                source + size_t(row) * sourceStride, width);
}
struct Scratch {
    uint8_t *images[3] = {};
    uint8_t *depth = nullptr;
    ~Scratch() {
        for (uint8_t *image : images) free(image);
        free(depth);
    }
};
bool validGeometry(const QCamera3DualBokehAlgo::Geometry &g,
        const QCamera3DualBokehAlgo::Image &wide,
        const QCamera3DualBokehAlgo::Image &tele) {
    return g.wideWidth >= wide.width && g.wideHeight >= wide.height &&
            g.teleWidth >= tele.width && g.teleHeight >= tele.height &&
            g.wideWidth <= 16384 && g.wideHeight <= 16384 &&
            g.teleWidth <= 16384 && g.teleHeight <= 16384 &&
            std::isfinite(g.wideMaxFov) && g.wideMaxFov > 0 && g.wideMaxFov < 180 &&
            uint64_t(g.wideWidth) * wide.height == uint64_t(g.wideHeight) * wide.width &&
            uint64_t(g.teleWidth) * tele.height == uint64_t(g.teleHeight) * tele.width;
}
} // namespace

bool QCamera3DualBokehAlgo::isAvailable() {
    Lock lock;
    Api api;
    return api.valid();
}

bool QCamera3DualBokehAlgo::validCalibration(const void *calibration, size_t length) {
    if (!calibration || length != kCalibrationSize) return false;
    const uint8_t *data = static_cast<const uint8_t *>(calibration);
    // All raw fields read by the stock converter at 0xe488 are finite. Focal
    // lengths and principal points must describe two actual cameras. This is
    // structural validation, not verification of a unit's fuse binding/quality.
    const size_t fields[] = {0x0c, 0x28, 0xa0, 0xa4, 0xa8, 0xac, 0xb0, 0xb4,
            0xd8, 0xdc, 0xe0, 0xe4, 0xe8, 0xec};
    for (size_t offset : fields)
        if (!std::isfinite(readFloat(data, offset))) return false;
    return readFloat(data, 0x0c) > 0 && readFloat(data, 0x28) > 0 &&
            readShort(data, 0x10) > 0 && readShort(data, 0x12) > 0 &&
            readShort(data, 0x2c) > 0 && readShort(data, 0x2e) > 0;
}

int32_t QCamera3DualBokehAlgo::loadCalibration(UnitCalibration &result) {
    const CalibrationFiles files = {
        "/mnt/vendor/persist/camera/dual_cam_stero_otp.bin",
        "/mnt/vendor/persist/dual_camera_calibration/calibrationOutput.bin",
        "/mnt/vendor/persist/dual_camera_calibration/fuseId.bin",
        "/proc/fuseid"
    };
    return loadCalibration(files, result);
}

int32_t QCamera3DualBokehAlgo::loadCalibration(const CalibrationFiles &files,
        UnitCalibration &result) {
    result = UnitCalibration{};
    // Stock compares two zero-initialized 32-byte text prefixes with strcmp.
    // Reserve a terminator so a full prefix cannot read outside our storage.
    uint8_t savedFuse[33] = {}, currentFuse[33] = {};
    UnitCalibration candidate;
    if (readFile(files.calibratedOutput, candidate.data, sizeof(candidate.data)) == 0 &&
            validCalibration(candidate.data, sizeof(candidate.data))) {
        bool savedOpened = false;
        readFile(files.savedFuse, savedFuse, 32, &savedOpened);
        bool useOutput = true;
        if (savedOpened) {
            readFile(files.currentFuse, currentFuse, 32);
            useOutput = strcmp(reinterpret_cast<const char *>(savedFuse),
                    reinterpret_cast<const char *>(currentFuse)) == 0;
        }
        // Missing saved fuse does not disqualify calibrationOutput in stock.
        // If the saved fuse exists but the current file does not, stock compares
        // its saved prefix with the empty current prefix and usually selects OTP.
        if (useOutput) {
            candidate.source = UnitCalibration::CALIBRATED_OUTPUT;
            result = candidate;
            return 0;
        }
    }
    int32_t rc = readFile(files.moduleOtp, candidate.data, sizeof(candidate.data));
    if (rc != 0) return rc;
    if (!validCalibration(candidate.data, sizeof(candidate.data))) return -EINVAL;
    candidate.source = UnitCalibration::MODULE_OTP;
    result = candidate;
    return 0;
}

namespace {
int32_t processStereo(const QCamera3DualBokehAlgo::Image &wide,
        const QCamera3DualBokehAlgo::Image &tele,
        const QCamera3DualBokehAlgo::Image &dst,
        const QCamera3DualBokehAlgo::Geometry &geometry,
        const QCamera3DualBokehAlgo::Focus &focus,
        const void *calibration, size_t calibrationLength,
        const std::atomic<bool> *cancelFlag) {
    using Image = QCamera3DualBokehAlgo::Image;
    if (!QCamera3HdrAlgo::validImage(wide) || !QCamera3HdrAlgo::validImage(tele) ||
            !QCamera3HdrAlgo::validImage(dst) || wide.width != dst.width ||
            wide.height != dst.height || !validGeometry(geometry, wide, tele) ||
            !QCamera3DualBokehAlgo::validCalibration(calibration, calibrationLength) || focus.x < 0 ||
            focus.y < 0 || uint32_t(focus.x) >= wide.width ||
            uint32_t(focus.y) >= wide.height || focus.blurLevel < 0 ||
            focus.blurLevel > QCamera3DualBokehAlgo::kMaxBlurLevel)
        return -EINVAL;
    if (cancelled(cancelFlag)) return -ECANCELED;

    Scratch scratch;
    ArcImage images[3] = {};
    uint64_t total = 0;
    for (unsigned i = 0; i < 3; ++i) {
        const Image &source = i == 1 ? tele : wide;
        const uint32_t stride = (source.width + 63) & ~63U;
        const uint64_t ySize = uint64_t(stride) * source.height;
        const uint64_t length = ySize + uint64_t(stride) * (source.height / 2);
        total += length;
        if (length > SIZE_MAX || total > kScratchLimit) return -EOVERFLOW;
        scratch.images[i] = static_cast<uint8_t *>(calloc(1, size_t(length)));
        if (!scratch.images[i]) return -ENOMEM;
        ArcImage &image = images[i];
        image.format = 0x802;
        image.width = source.width;
        image.height = source.height;
        image.planes[0] = scratch.images[i];
        image.planes[1] = scratch.images[i] + size_t(ySize);
        image.pitches[0] = image.pitches[1] = stride;
        if (i != 2) {
            copyPlane(image.planes[0], stride, source.data + source.yOffset,
                    source.yStride, source.width, source.height);
            copyPlane(image.planes[1], stride, source.data + source.uvOffset,
                    source.uvStride, source.width, source.height / 2);
        }
    }
    const ArcImage expectedOutput = images[2];
    uint8_t calibrationCopy[kCalibrationSize];
    memcpy(calibrationCopy, calibration, sizeof(calibrationCopy));
    Calibration calibrationRecord = {calibrationCopy, int32_t(sizeof(calibrationCopy))};
    CameraImageInfo imageInfo = {int32_t(geometry.wideWidth), int32_t(geometry.wideHeight),
            int32_t(geometry.teleWidth), int32_t(geometry.teleHeight)};
    DisparityParam disparity = {270, geometry.wideMaxFov, nullptr, nullptr, 0};
    RefocusParam refocus = {focus.x, focus.y, focus.blurLevel, 0, 0, 0, 0, 0};

    Lock lock;
    if (cancelled(cancelFlag)) return -ECANCELED;
    Api api;
    if (!api.valid()) return -ENOSYS;
    void *handle = nullptr;
    int32_t rc = api.init(&handle, 1); // Stock ARC_DCIR capture initialization.
    if (rc == 0 && !handle) rc = -EIO;
#define RUN(call) do { if (rc == 0) { if (cancelled(cancelFlag)) rc = -ECANCELED; \
        else rc = (call); } } while (0)
    RUN(api.setCalibration(handle, &calibrationRecord));
    RUN(api.reset(handle));
    RUN(api.setGeometry(handle, &imageInfo));
    RUN(api.calculate(handle, &images[0], &images[1], &disparity));
    int32_t depthLength = 0;
    RUN(api.depthSize(handle, &depthLength));
    if (rc == 0 && (depthLength <= 0 || depthLength > kDepthLimit ||
            total + uint32_t(depthLength) > kScratchLimit)) rc = -EOVERFLOW;
    if (rc == 0) {
        scratch.depth = static_cast<uint8_t *>(calloc(1, size_t(depthLength)));
        if (!scratch.depth) rc = -ENOMEM;
    }
    RUN(api.depthData(handle, scratch.depth));
    RUN(api.process(handle, scratch.depth, depthLength, &images[0], &refocus, &images[2]));
#undef RUN
    // Stock Uninit dereferences a non-null handle pointer without checking its
    // pointee. Skip it for null handles; clean partial failed initialization.
    if (handle) {
        const int32_t cleanup = api.uninit(&handle);
        if (rc == 0 && cleanup != 0) rc = cleanup;
    }
    if (cancelled(cancelFlag)) return -ECANCELED;
    if (rc != 0) return rc;
    if (memcmp(&images[2], &expectedOutput, sizeof(ArcImage)) != 0) return -EPROTO;
    copyPlane(dst.data + dst.yOffset, dst.yStride, images[2].planes[0],
            images[2].pitches[0], dst.width, dst.height);
    copyPlane(dst.data + dst.uvOffset, dst.uvStride, images[2].planes[1],
            images[2].pitches[1], dst.width, dst.height / 2);
    return 0;
}
} // namespace

int32_t QCamera3DualBokehAlgo::process(const Image &wide, const Image &tele,
        const Image &destination, const Geometry &geometry, const Focus &focus,
        const void *calibration, size_t length, const std::atomic<bool> *cancelFlag) {
    return processStereo(wide, tele, destination, geometry, focus,
            calibration, length, cancelFlag);
}

} // namespace qcamera
