/* SPDX-License-Identifier: BSD-3-Clause */
#include "CameraGeometry.h"
#include <dlfcn.h>
#include <cstring>
#include <stdint.h>
namespace {
using Original = int (*)(void *, uint32_t, void *, void *, uint32_t);
Original original() {
    static void *library = dlopen("libmmcamera2_isp_modules.so", RTLD_NOW | RTLD_LOCAL);
    static auto function = library ? reinterpret_cast<Original>(
            dlsym(library, "isp_resource_calculate_roi_map")) : nullptr;
    return function;
}
}
extern "C" int isp_resource_calculate_roi_map(void *resource, uint32_t isp,
        void *zooms, void *crop, uint32_t identity) {
    const auto function = original();
    if (!function) return 0;
    const int status = function(resource, isp, zooms, crop, identity);
    if (status && zooms && crop) {
        uint32_t native;
        memcpy(&native, crop, sizeof(native));
        const auto *record = static_cast<const uint8_t *>(zooms);
        for (unsigned i = 0; i < 8; ++i, record += 0x50) {
            uint32_t candidate;
            memcpy(&candidate, record, sizeof(candidate));
            if (candidate == identity) {
                oppo_camera_geometry_record_isp(identity, native, record);
                break;
            }
        }
    }
    return status;
}
