/*
 * Android 16 Camera3FakeStream compatibility.
 *
 * Camera3Device injects a fake OUTPUT stream when a non-virtual camera session
 * is configured with no real outputs (framework zero-stream workaround).  The
 * geometry/usage below are the AOSP Camera3FakeStream constants.  A fake-only
 * configuration must not reach the camera daemon: it is accepted as an empty
 * session transition, while ordinary 320x240 preview requests remain subject
 * to the regular advertised-size checks.
 */
#ifndef QCAMERA3_FAKE_STREAM_RULES_H
#define QCAMERA3_FAKE_STREAM_RULES_H

#include <cstddef>
#include <cstdint>

namespace QCamera3FakeStreamRules {

constexpr int32_t FAKE_WIDTH = 320;
constexpr int32_t FAKE_HEIGHT = 240;
constexpr int32_t FAKE_FORMAT = 0x22;   // HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED
constexpr uint64_t FAKE_USAGE = 0x800;  // GRALLOC_USAGE_HW_COMPOSER
constexpr int FAKE_ROTATION = 0;        // CAMERA3_STREAM_ROTATION_0
constexpr int FAKE_DATA_SPACE = 0;      // HAL_DATASPACE_UNKNOWN
constexpr int STREAM_TYPE_OUTPUT = 0;   // CAMERA3_STREAM_OUTPUT

inline bool isFakeStreamOnly(
        size_t numStreams,
        int streamType,
        int format,
        int32_t width,
        int32_t height,
        int rotation,
        uint64_t usage,
        int dataSpace) {
    return numStreams == 1 &&
            streamType == STREAM_TYPE_OUTPUT &&
            format == FAKE_FORMAT &&
            width == FAKE_WIDTH &&
            height == FAKE_HEIGHT &&
            rotation == FAKE_ROTATION &&
            usage == FAKE_USAGE &&
            dataSpace == FAKE_DATA_SPACE;
}

}  // namespace QCamera3FakeStreamRules

#endif  // QCAMERA3_FAKE_STREAM_RULES_H
