/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_GYRO_VSTAB_H
#define QCAMERA3_GYRO_VSTAB_H

#include "QCamera3HdrAlgo.h"

namespace qcamera {

// HAL3-native gyro video stabilisation (VStab) stage. It replaces the stock
// HAL1 libmms_hal_vstab.so pipeline: this class owns the crop/offset policy, a
// bounded gyro integrator (bias and intentional-motion estimation, hard drift
// bounds) and a bounded NV12/NV21 bilinear warper. When the stock algorithm
// core libmms_gyro_vstab.so is usable it is dlopen'ed here and its per-frame
// warp grid is reduced to the crop factor and sampling offset this warper
// applies; otherwise the same numbers come from the native integrator. The
// result always states which path produced the pixels.
//
// Gyro route. The port's only real gyro client is QCamera3MmsGyroSource whose
// sample stream is delivered only to a QCamera3MmsVstab sink
// (QCamera3MmsGyroSource::start(QCamera3MmsVstab &, Notify, void *)) and that
// sink does not expose its grid. This stage therefore consumes the same sensor
// samples through addGyro(), whose arguments and validation deliberately mirror
// QCamera3MmsVstab::addGyro: CLOCK_BOOTTIME nanoseconds, rad/s in the Android
// sensor axes (+X right, +Y up, +Z out of the display), strictly increasing
// timestamps. No sample stream exists inside this stage: with no gyro data it
// refuses every frame and never claims stabilisation.
//
// Sign convention (verified numerically by tests/native/gyro_vstab_test.cpp):
//   theta = integral of the gyro rate over the frame window, high-pass filtered.
//   shiftX = focalLengthPx * tan(theta_y)  -> sampling window moves right
//   shiftY = focalLengthPx * tan(theta_x)  -> sampling window moves down
// The warp computes dst(x,y) = src(x + shiftX, y + shiftY) with a bilinear
// filter, so a positive reported offset moves picture content left/up by that
// many source pixels. Yawing the device left (positive theta_y) therefore moves
// the sampled window right and cancels the rightward scene motion.
class QCamera3GyroVstab {
public:
    using Image = QCamera3HdrAlgo::Image;

    // Which implementation produced the frame. Reported in Result::path and
    // Result::detail[0].
    enum Path : int32_t { PATH_NONE = 0, PATH_NATIVE = 1, PATH_STOCK = 2, PATH_AUTO = 3 };

    // Why a frame was or was not stabilised; Result::reason.
    enum Reason : int32_t {
        REASON_OK = 0,             // real gyro data, warp applied
        REASON_BYPASS = 1,         // strength == 0: explicit OFF, byte copy
        REASON_DISABLED = 2,       // setEnabled(false): explicit OFF, byte copy
        REASON_NO_GYRO = 3,        // no sample retained at all
        REASON_GYRO_STALE = 4,     // newest sample predates the frame/window hole
        REASON_OUT_OF_WINDOW = 5,  // window start is older than retained history
        REASON_FRAME_BACKWARDS = 6,// frame timestamp did not advance
        REASON_CORE_UNVERIFIED = 7,// stock path requested but core not usable
    };

    // Negative status codes returned by process() (never 0 on refusal):
    //   -ENODATA    REASON_NO_GYRO
    //   -ETIMEDOUT  REASON_GYRO_STALE
    //   -ERANGE     REASON_OUT_OF_WINDOW
    //   -EINVAL     REASON_FRAME_BACKWARDS, malformed image or options
    //   -ENOSYS     REASON_CORE_UNVERIFIED
    //   -ECANCELED  cancelled; destination untouched
    //   -E2BIG      frame larger than the documented scratch bound
    // On every negative status applied == 0. The destination is written only
    // when the frame is really stabilised, when strength == 0 / disabled
    // (documented OFF behaviour, byte-identical copy), or on a refusal with
    // Options::passthrough == 1 (documented pass-through copy of the source).
    struct Options {
        int32_t strength = 0;       // 0..100; 0 = explicit OFF
        int32_t maxCropPercent = 0; // 0..kMaxCropPercent reserved margin (FOV loss)
        int64_t windowNs = 0;       // kMinWindowNs..kMaxWindowNs gyro span per frame
        int64_t timestampNs = 0;    // frame sensor timestamp, CLOCK_BOOTTIME
        uint32_t frameNumber = 0;
        // Real calibration in destination pixels:
        // f_px = focal_mm * output_width_px / sensor_active_width_mm
        // (ANDROID_LENS_INFO_AVAILABLE_FOCAL_LENGTHS + SENSOR_INFO_PHYSICAL_SIZE).
        // strength > 0 requires a value in [kMinFocalPx, kMaxFocalPx]: an angle
        // cannot be converted to pixels without it, so 0 is refused, never guessed.
        float focalLengthPx = 0.0f;
        // Real sensor values for the stock engine's per-frame structure:
        // exposure time in ns (0 = unknown) and the current zoom (>= 1.0).
        // The native integrator does not use them.
        int64_t exposureNs = 0;
        float zoom = 1.0f;
        int32_t path = PATH_AUTO;   // PATH_AUTO / PATH_NATIVE / PATH_STOCK
        int32_t passthrough = 0;    // 1 = copy source on refusal
        static bool valid(const Options &o);
    };

    struct Result {
        int32_t status = 0;        // 0 pass; negative errno otherwise
        int32_t applied = 0;       // effective strength; 0 = pixels unchanged
        uint32_t frameNumber = 0;  // frame whose processing wrote this
        // detail[0] = Path, detail[1] = cropFactorPermille,
        // detail[2] = offsetXMilli, detail[3] = offsetYMilli.
        int32_t detail[4] = {};

        int32_t path = PATH_NONE;
        int32_t reason = REASON_OK;
        // Fraction of the source frame retained in the output, per mille.
        // 1000 = full field of view, 920 = the documented 8% margin.
        int32_t cropFactorPermille = 1000;
        // Applied sampling shift in source pixels, thousandths of a pixel.
        // Positive offsetX moves picture content left, positive offsetY up.
        int32_t offsetXMilli = 0, offsetYMilli = 0;
        int32_t gyroCount = 0;     // samples integrated for this frame
        int32_t coreStatus = 0;    // stock vStabWarpGrid return (0 when unused)
        int32_t clamped = 0;       // 1 when the crop/offset bound clipped the warp
        // High-pass (bias + intentional motion) estimate the stage removed,
        // milli-radians per second, per gyro axis. Diagnostics only.
        int32_t biasMilliRadPerSec[3] = {};
    };

    QCamera3GyroVstab();
    ~QCamera3GyroVstab();
    static const char *name();          // "GyroVstab"
    static bool isAvailable();          // native warper is always available
    static bool stockCoreAvailable();   // libmms_gyro_vstab.so + all five names
    static bool supports(uint32_t width, uint32_t height);
    static bool validOptions(const Options &o);
    // Opens libmms_gyro_vstab.so (RTLD_NOW | RTLD_LOCAL, entry points resolved
    // lazily here) and creates one engine from a real VStab profile file. The
    // stock grid is still only a candidate source of crop/offset: a core error
    // falls back to the native integrator and the result says PATH_NATIVE.
    int32_t configureStock(const char *profilePath, uint32_t width, uint32_t height);
    void setEnabled(bool on);
    bool enabled() const;
    // Sample entry; see the gyro route note above. Returns -EINVAL for a
    // non-finite rate or a timestamp that did not advance.
    int32_t addGyro(int64_t timestampNs, const float angularVelocity[3]);
    size_t gyroSamples() const;
    // Single frame entry point. Transactional: on any negative status the
    // destination bytes are unchanged unless Options::passthrough == 1.
    int32_t process(const Image &source, const Image &destination, const Options &options,
            Result &result, const std::atomic<bool> *cancelled = nullptr);
    // Drops the gyro history, the estimators and the stock engine. Only after
    // the owning worker joined.
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
