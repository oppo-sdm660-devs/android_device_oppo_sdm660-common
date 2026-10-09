/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_GYRO_VSTAB_H
#define QCAMERA3_GYRO_VSTAB_H

#include "QCamera3HdrAlgo.h"

namespace qcamera {

// HAL3-native gyro video stabilisation; refuses frames without real gyro data.
// addGyro() samples are CLOCK_BOOTTIME ns, rad/s in the Android sensor axes.
// Warp: dst(x,y) = src(x + shiftX, y + shiftY); positive offsets move content left/up.
class QCamera3GyroVstab {
public:
    using Image = QCamera3HdrAlgo::Image;

    enum Path : int32_t { PATH_NONE = 0, PATH_NATIVE = 1, PATH_STOCK = 2, PATH_AUTO = 3 };

    enum Reason : int32_t {
        REASON_OK = 0,
        REASON_BYPASS = 1,
        REASON_DISABLED = 2,
        REASON_NO_GYRO = 3,
        REASON_GYRO_STALE = 4,
        REASON_OUT_OF_WINDOW = 5,
        REASON_FRAME_BACKWARDS = 6,
        REASON_CORE_UNVERIFIED = 7,
    };

    // Refusals: -ENODATA no gyro, -ETIMEDOUT stale, -ERANGE window hole,
    // -EINVAL bad input, -ENOSYS stock core unusable, -ECANCELED, -E2BIG.
    // Destination is written only on success, on OFF, or with passthrough == 1.
    struct Options {
        int32_t strength = 0;       // 0..100; 0 = explicit OFF
        int32_t maxCropPercent = 0; // 0..kMaxCropPercent reserved margin (FOV loss)
        int64_t windowNs = 0;       // kMinWindowNs..kMaxWindowNs gyro span per frame
        int64_t timestampNs = 0;    // frame sensor timestamp, CLOCK_BOOTTIME
        uint32_t frameNumber = 0;
        // Calibration in destination pixels: f_px = focal_mm * output_width_px /
        // sensor_active_width_mm; required in [kMinFocalPx, kMaxFocalPx] when strength > 0.
        float focalLengthPx = 0.0f;
        // Stock-core inputs: exposure ns (0 = unknown) and zoom (>= 1.0).
        int64_t exposureNs = 0;
        float zoom = 1.0f;
        int32_t path = PATH_AUTO;
        int32_t passthrough = 0;    // 1 = copy source on refusal
        static bool valid(const Options &o);
    };

    struct Result {
        int32_t status = 0;        // 0 pass; negative errno otherwise
        int32_t applied = 0;       // effective strength; 0 = pixels unchanged
        uint32_t frameNumber = 0;
        // detail[0] = Path, detail[1] = cropFactorPermille, detail[2] = offsetXMilli,
        // detail[3] = offsetYMilli.
        int32_t detail[4] = {};

        int32_t path = PATH_NONE;
        int32_t reason = REASON_OK;
        // Retained source fraction per mille (1000 = full field of view).
        int32_t cropFactorPermille = 1000;
        // Sampling shift in thousandths of a source pixel; positive moves content left/up.
        int32_t offsetXMilli = 0, offsetYMilli = 0;
        int32_t gyroCount = 0;
        int32_t coreStatus = 0;    // stock vStabWarpGrid return (0 when unused)
        int32_t clamped = 0;       // 1 when the crop/offset bound clipped the warp
        // Removed bias estimate, milli-radians per second per axis; diagnostics only.
        int32_t biasMilliRadPerSec[3] = {};
    };

    QCamera3GyroVstab();
    ~QCamera3GyroVstab();
    static const char *name();
    static bool isAvailable();
    static bool stockCoreAvailable();   // libmms_gyro_vstab.so with all five entry points
    static bool supports(uint32_t width, uint32_t height);
    static bool validOptions(const Options &o);
    // Opens libmms_gyro_vstab.so and creates one engine from a VStab profile file.
    int32_t configureStock(const char *profilePath, uint32_t width, uint32_t height);
    void setEnabled(bool on);
    bool enabled() const;
    // -EINVAL for a non-finite rate or a timestamp that did not advance.
    int32_t addGyro(int64_t timestampNs, const float angularVelocity[3]);
    size_t gyroSamples() const;
    // On any negative status the destination is unchanged unless passthrough == 1.
    int32_t process(const Image &source, const Image &destination, const Options &options,
            Result &result, const std::atomic<bool> *cancelled = nullptr);
    // Drops gyro history, estimators and the stock engine.
    void reset();

    static constexpr int32_t kMaxStrength = 100;
    static constexpr int32_t kMaxCropPercent = 20;
    static constexpr int64_t kMinWindowNs = 1000000;      // 1 ms
    static constexpr int64_t kMaxWindowNs = 100000000;    // 100 ms
    static constexpr float kMinFocalPx = 16.0f;
    static constexpr float kMaxFocalPx = 65536.0f;
    static constexpr uint32_t kMaxDimension = 4096;
    static constexpr size_t kMaxScratchBytes = 16U * 1024U * 1024U;

private:
    struct Impl;
    Impl *mImpl;
    QCamera3GyroVstab(const QCamera3GyroVstab &) = delete;
    QCamera3GyroVstab &operator=(const QCamera3GyroVstab &) = delete;
};

} // namespace qcamera
#endif
