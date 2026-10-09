/* SPDX-License-Identifier: BSD-3-Clause */
#include "CameraGeometry.h"
#include <dlfcn.h>
#include <cstring>
#include <errno.h>
#include <stdint.h>
namespace {
using Original = int (*)(void *, void *, void *);
void *library() {
    static void *handle = dlopen("libmmcamera2_cpp_module.so", RTLD_NOW | RTLD_LOCAL);
    return handle;
}
Original original() {
    static auto function = library() ? reinterpret_cast<Original>(
            dlsym(library(), "cpp_module_util_post_crop_info")) : nullptr;
    return function;
}
uint32_t word(const void *source, unsigned offset) {
    uint32_t value;
    memcpy(&value, static_cast<const uint8_t *>(source) + offset, sizeof(value));
    return value;
}
uint8_t byte(const void *source, unsigned offset) {
    return static_cast<const uint8_t *>(source)[offset];
}
}
extern "C" int cpp_module_util_post_crop_info(void *module, void *hardware, void *stream) {
    const auto function = original();
    if (!function) return -ENODEV;
    const int status = function(module, hardware, stream);
    if (status == 0 && hardware && stream && word(hardware, 4) == word(stream, 0x5340)) {
        const uint32_t identity = word(hardware, 4);
        const uint32_t type = word(stream, 0x5348);
        if (type == 1 || (type == 5 && oppo_camera_geometry_has_requested_cpp(identity)))
            oppo_camera_geometry_record_cpp(identity, word(hardware, 0), hardware);
    }
    return status;
}

extern "C" uint32_t cpp_module_util_decide_proc_frame_per_stream(void *module,
        void *session, void *stream, uint32_t nativeFrame) {
    using Decide = uint32_t (*)(void *, void *, void *, uint32_t);
    using Valid = uint32_t (*)(void *, uint32_t, uint32_t, uint32_t, uint32_t);
    static const auto decide = library() ? reinterpret_cast<Decide>(
            dlsym(library(), "cpp_module_util_decide_proc_frame_per_stream")) : nullptr;
    static const auto valid = library() ? reinterpret_cast<Valid>(
            dlsym(library(), "cpp_module_get_frame_valid")) : nullptr;
    if (!decide) return 0;
    const uint32_t result = decide(module, session, stream, nativeFrame);
    if (result || !module || !session || !stream || !valid) return result;
    const uint32_t identity = word(stream, 0x5340);
    if (word(stream, 0x5348) != 5 || word(stream, 0x534c) != 1 ||
            word(session, 0x8890) != 3 || byte(session, 0x92e8) != 1 ||
            word(session, 0x92fc) == 1 || !oppo_camera_geometry_has_requested_cpp(identity))
        return result;
    // This pin skips slave outputs before checking their HALv3 PCR. Keep the
    // original single-consumer PCR lookup; no shared role/sticky bit is changed.
    return valid(module, identity, nativeFrame, identity & 0xffff, 5);
}
