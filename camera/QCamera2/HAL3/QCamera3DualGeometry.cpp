/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3DualGeometry.h"

#include <cmath>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <openssl/sha.h>
#include <sys/stat.h>
#include <unistd.h>

namespace qcamera {
namespace {
using Geometry = QCamera3DualGeometry;
bool sameRect(const Geometry::Rect &a, const Geometry::Rect &b) {
    return a.left == b.left && a.top == b.top && a.width == b.width && a.height == b.height;
}
bool contained(const Geometry::Rect &a, const Geometry::Rect &b) {
    return b.left >= 0 && b.top >= 0 && b.width > 0 && b.height > 0 &&
            a.left >= b.left && a.top >= b.top && a.width > 0 && a.height > 0 &&
            int64_t(a.left) + a.width <= int64_t(b.left) + b.width &&
            int64_t(a.top) + a.height <= int64_t(b.top) + b.height;
}
bool dimension(uint32_t w, uint32_t h) {
    return w && h && w <= 16384 && h <= 16384 && !((w | h) & 1);
}
Geometry::Rect imageRoi(uint32_t sensorWidth, uint32_t sensorHeight,
        uint32_t imageWidth, uint32_t imageHeight) {
    uint32_t width = sensorWidth, height = sensorHeight;
    if (uint64_t(width) * imageHeight > uint64_t(height) * imageWidth)
        width = uint64_t(height) * imageWidth / imageHeight;
    else
        height = uint64_t(width) * imageHeight / imageWidth;
    width &= ~1U; height &= ~1U;
    return {int32_t(((sensorWidth - width) / 2) & ~1U),
            int32_t(((sensorHeight - height) / 2) & ~1U), int32_t(width), int32_t(height)};
}
bool nonzero(const uint8_t *bytes, size_t count) {
    for (size_t i = 0; i < count; ++i) if (bytes[i]) return true;
    return false;
}
bool validInfo(const Geometry::SensorInfo &info) {
    const Geometry::Rect pixels = {0, 0, int32_t(info.pixelWidth), int32_t(info.pixelHeight)};
    return dimension(info.pixelWidth, info.pixelHeight) && contained(info.activeArray, pixels) &&
            info.mountAngle == 90 && std::isfinite(info.focalLength) && info.focalLength > 0 &&
            std::isfinite(info.horizontalFov) && info.horizontalFov > 0 && info.horizontalFov < 180 &&
            std::isfinite(info.verticalFov) && info.verticalFov > 0 && info.verticalFov < 180;
}
bool sameInfo(const Geometry::SensorInfo &a, const Geometry::SensorInfo &b) {
    return a.pixelWidth == b.pixelWidth && a.pixelHeight == b.pixelHeight &&
            sameRect(a.activeArray, b.activeArray) && a.mountAngle == b.mountAngle &&
            a.focalLength == b.focalLength && a.horizontalFov == b.horizontalFov &&
            a.verticalFov == b.verticalFov;
}
struct Reader {
    const uint8_t *data;
    uint32_t integer() {
        const uint32_t result = uint32_t(data[0]) | uint32_t(data[1]) << 8 |
                uint32_t(data[2]) << 16 | uint32_t(data[3]) << 24;
        data += 4; return result;
    }
    int64_t duration() { const uint64_t lo = integer(), hi = integer(); return int64_t(lo | hi << 32); }
    float number() { const uint32_t bits = integer(); float result; memcpy(&result, &bits, 4); return result; }
    Geometry::Rect rect() {
        Geometry::Rect result;
        result.left = int32_t(integer()); result.top = int32_t(integer());
        result.width = int32_t(integer()); result.height = int32_t(integer()); return result;
    }
};
bool validProfile(const Geometry::Profile &profile) {
    if ((profile.calibrationSource != Geometry::Calibration::MODULE_OTP &&
            profile.calibrationSource != Geometry::Calibration::CALIBRATED_OUTPUT) ||
            !nonzero(profile.calibrationSha256, 32)) return false;
    QCamera3DualCapture::Config config = {};
    for (unsigned i = 0; i < 2; ++i) {
        const auto &sensor = profile.sensors[i];
        const auto &capture = sensor.capture;
        const Geometry::Rect output = {0, 0, int32_t(sensor.outputWidth), int32_t(sensor.outputHeight)};
        if (!validInfo(sensor.info) || !dimension(sensor.outputWidth, sensor.outputHeight) ||
                !contained(capture.referenceCrop, output) ||
                !contained(capture.activeArrayCrop, sensor.info.activeArray) ||
                !dimension(capture.imageWidth, capture.imageHeight) ||
                !capture.requireStreamCrop ||
                !contained(capture.streamCrop, {0, 0, int32_t(capture.imageWidth), int32_t(capture.imageHeight)}) ||
                !contained(capture.streamRoiMap, output)) return false;
        config.sensors[i] = capture; config.sensors[i].cameraHandle = i + 1;
    }
    config.wideMaxFov = fmaxf(profile.sensors[0].info.horizontalFov, profile.sensors[0].info.verticalFov);
    config.maxTimestampDeltaNs = profile.maxTimestampDeltaNs; config.timeoutNs = profile.timeoutNs;
    return QCamera3DualCapture::validConfig(config);
}
} // namespace

const char *QCamera3DualGeometry::installedPath() {
    return "/mnt/vendor/persist/camera/hal3_dual_geometry.bin";
}
int32_t QCamera3DualGeometry::load(const Calibration &calibration, Profile &result) {
    return load(installedPath(), calibration, result);
}
bool QCamera3DualGeometry::matchesCalibration(const Profile &profile, const Calibration &calibration) {
    if (profile.calibrationSource != calibration.source ||
            !QCamera3DualBokehAlgo::validCalibration(calibration.data, sizeof(calibration.data))) return false;
    uint8_t digest[SHA256_DIGEST_LENGTH];
    return SHA256(calibration.data, sizeof(calibration.data), digest) &&
            memcmp(profile.calibrationSha256, digest, sizeof(digest)) == 0;
}
int32_t QCamera3DualGeometry::load(const char *path, const Calibration &calibration, Profile &result) {
    result = Profile{};
    if (!path) return -EINVAL;
    const int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
    if (fd < 0) return -errno;
    uint8_t bytes[kFileSize] = {};
    struct stat info = {};
    int32_t rc = 0;
    // Ignore writable files/symlinks; overrides come from privileged tooling.
    if (fstat(fd, &info) != 0) rc = -errno;
    else if (!S_ISREG(info.st_mode) || info.st_size != int64_t(sizeof(bytes)) ||
            info.st_uid != 0 || (info.st_mode & (S_IWGRP | S_IWOTH))) rc = -EPERM;
    size_t offset = 0;
    while (rc == 0 && offset < sizeof(bytes)) {
        const ssize_t count = read(fd, bytes + offset, sizeof(bytes) - offset);
        if (count > 0) offset += size_t(count);
        else if (count == 0) rc = -EMSGSIZE;
        else if (errno != EINTR) rc = -errno;
    }
    close(fd);
    if (rc != 0) return rc;
    Profile candidate;
    rc = decode(bytes, sizeof(bytes), calibration, candidate);
    if (rc != 0) return rc;
    result = candidate; return 0;
}
int32_t QCamera3DualGeometry::fromCapabilities(const Calibration &calibration,
        const SensorInfo sensors[2], Profile &result) {
    result = {};
    if (!sensors || calibration.source == Calibration::NONE ||
            !QCamera3DualBokehAlgo::validCalibration(calibration.data, sizeof(calibration.data)))
        return -EINVAL;
    Profile candidate;
    candidate.calibrationSource = calibration.source;
    if (!SHA256(calibration.data, sizeof(calibration.data), candidate.calibrationSha256)) return -EINVAL;
    // ARC_DCIR_SetCameraImageInfo reference sizes from the stock HAL; the raw
    // calibration record has no reference-size fields.
    const uint32_t referenceWidths[] = {3264, 5120};
    const uint32_t referenceHeights[] = {2448, 3848};
    for (unsigned i = 0; i < 2; ++i) {
        if (!validInfo(sensors[i]) || sensors[i].pixelWidth < referenceWidths[i] ||
                sensors[i].pixelHeight < referenceHeights[i]) return -ERANGE;
        auto &sensor = candidate.sensors[i];
        sensor.info = sensors[i];
        sensor.outputWidth = sensors[i].pixelWidth; sensor.outputHeight = sensors[i].pixelHeight;
        auto &capture = sensor.capture;
        capture.referenceWidth = capture.imageWidth = referenceWidths[i];
        capture.referenceHeight = capture.imageHeight = referenceHeights[i];
        capture.referenceCrop = {0, 0, int32_t(sensor.outputWidth), int32_t(sensor.outputHeight)};
        capture.activeArrayCrop = sensors[i].activeArray;
        capture.streamCrop = {0, 0, int32_t(capture.imageWidth), int32_t(capture.imageHeight)};
        capture.streamRoiMap = imageRoi(sensor.outputWidth, sensor.outputHeight,
                capture.imageWidth, capture.imageHeight);
        capture.requireStreamCrop = true;
    }
    candidate.maxTimestampDeltaNs = 10000000; // A mismatched pair falls back to the main JPEG.
    candidate.timeoutNs = 3000000000LL;
    candidate.runtimeGeometry = true;
    if (!validProfile(candidate)) return -EINVAL;
    result = candidate; return 0;
}
int32_t QCamera3DualGeometry::decode(const uint8_t *bytes, size_t length,
        const Calibration &calibration, Profile &result) {
    result = Profile{};
    if (!bytes || length != kFileSize) return -EMSGSIZE;
    if (memcmp(bytes, "R11DGM01", 8) != 0) return -EPROTO;
    Reader reader = {bytes + 8};
    if (reader.integer() != kFileSize) return -EPROTO;
    Profile candidate;
    candidate.calibrationSource = Calibration::Source(reader.integer());
    memcpy(candidate.calibrationSha256, reader.data, 32); reader.data += 32;
    memcpy(candidate.evidenceSha256, reader.data, 32); reader.data += 32;
    for (auto &sensor : candidate.sensors) {
        auto &cap = sensor.info; auto &capture = sensor.capture;
        cap.pixelWidth = reader.integer(); cap.pixelHeight = reader.integer(); cap.activeArray = reader.rect();
        cap.mountAngle = reader.integer(); cap.focalLength = reader.number();
        cap.horizontalFov = reader.number(); cap.verticalFov = reader.number();
        sensor.outputWidth = reader.integer(); sensor.outputHeight = reader.integer();
        capture.referenceWidth = reader.integer(); capture.referenceHeight = reader.integer();
        capture.referenceCrop = reader.rect(); capture.activeArrayCrop = reader.rect();
        capture.imageWidth = reader.integer(); capture.imageHeight = reader.integer();
        capture.streamCrop = reader.rect(); capture.streamRoiMap = reader.rect(); capture.requireStreamCrop = true;
    }
    candidate.maxTimestampDeltaNs = reader.duration(); candidate.timeoutNs = reader.duration();
    if (reader.data != bytes + length || !validProfile(candidate)) return -EINVAL;
    if (!matchesCalibration(candidate, calibration)) return -ESTALE;
    result = candidate; return 0;
}
bool QCamera3DualGeometry::matchesCapabilities(const Profile &profile, const SensorInfo sensors[2]) {
    if (!sensors || !validProfile(profile)) return false;
    for (unsigned i = 0; i < 2; ++i)
        if (!sameInfo(profile.sensors[i].info, sensors[i])) return false;
    return true;
}
int32_t QCamera3DualGeometry::bind(const Profile &profile, const Calibration &calibration,
        const SensorInfo sensors[2], const uint32_t handles[2], const uint32_t widths[2],
        const uint32_t heights[2], QCamera3DualCapture::Config &result) {
    result = {};
    if (!handles || !widths || !heights || !matchesCapabilities(profile, sensors)) return -EINVAL;
    if (!matchesCalibration(profile, calibration)) return -ESTALE;
    QCamera3DualCapture::Config candidate = {};
    for (unsigned i = 0; i < 2; ++i) {
        if (!dimension(widths[i], heights[i])) return -ERANGE;
        if (!profile.runtimeGeometry &&
                (widths[i] != profile.sensors[i].outputWidth || heights[i] != profile.sensors[i].outputHeight))
            return -ERANGE;
        candidate.sensors[i] = profile.sensors[i].capture; candidate.sensors[i].cameraHandle = handles[i];
        if (profile.runtimeGeometry) {
            if (widths[i] < candidate.sensors[i].imageWidth ||
                    heights[i] < candidate.sensors[i].imageHeight) return -ERANGE;
            candidate.sensors[i].referenceCrop = {0, 0, int32_t(widths[i]), int32_t(heights[i])};
            candidate.sensors[i].streamRoiMap = imageRoi(widths[i], heights[i],
                    candidate.sensors[i].imageWidth, candidate.sensors[i].imageHeight);
        }
    }
    candidate.wideMaxFov = fmaxf(sensors[0].horizontalFov, sensors[0].verticalFov);
    candidate.maxTimestampDeltaNs = profile.maxTimestampDeltaNs; candidate.timeoutNs = profile.timeoutNs;
    if (!QCamera3DualCapture::validConfig(candidate)) return -EINVAL;
    result = candidate; return 0;
}
} // namespace qcamera
