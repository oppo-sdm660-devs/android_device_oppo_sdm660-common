/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_VIDEO_EFFECTS_H
#define QCAMERA3_VIDEO_EFFECTS_H
#include "QCamera3HALHeader.h"
#include "QCamera3HdrAlgo.h"
#include <atomic>
#include <pthread.h>
namespace qcamera {
// Single-frame SDR effects for HAL3 preview and encoder outputs. No multiframe
// capture, HAL1 state object, HLG/HDR10 transfer function or 10-bit claim.
// The round-2 continuous stages (effects 5 and 7..9) and the continuous
// stage graph were deleted with the 2026-10-17 B2-b/B2-c batch; effect 1
// (rear beauty) and effect 3 (SDR tone) keep their legacy single-pass path.
class QCamera3VideoEffects {
public:
    using Image = QCamera3HdrAlgo::Image;
    static constexpr uint32_t kMaxPixels = 1920U * 1080U;
    // Upper bound accepted for a decoded focal length in destination pixels;
    // identical to QCamera3GyroVstab::kMaxFocalPx.
    static constexpr float kMaxFocalLengthPx = 65536.0f;
    struct Options {
        int32_t beauty = 0;       // 0..100; original ArcSoft single-image core
        int32_t tone = 0;         // 0..100; bounded SDR dynamic-range compression
        // Real calibration of the streaming output, destination pixels:
        // f_px = output_width / (2 * tan(horizontal_fov / 2)) from the decoded
        // capability. 0 = not decoded: the gyro stabiliser refuses, never guesses.
        float focalLengthPx = 0.0f;
        bool enabled() const { return beauty || tone; }
        // Single-pass effects handled by the legacy QCamera3VideoEffects path.
        bool legacy() const { return beauty || tone; }
    };
    static bool valid(const Options &options);
    static bool supports(uint32_t width, uint32_t height);
    // NV12/NV21 is explicit. Copy into bounded scratch and commit only after all
    // effects succeed. On error/cancellation the destination is unchanged.
    static int32_t process(const Image &input, bool inputNv21,
            const Image &output, bool outputNv21, const Options &options,
            const std::atomic<bool> *cancelled = nullptr);
};
}
#endif
