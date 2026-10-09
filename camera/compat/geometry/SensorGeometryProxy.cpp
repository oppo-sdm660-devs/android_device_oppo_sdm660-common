/* SPDX-License-Identifier: BSD-3-Clause */
#include "CameraGeometry.h"
#include <dlfcn.h>
namespace {
using Original = int (*)(void *, uint32_t, int, void *);
Original original() {
    static void *library = dlopen("libmmcamera2_sensor_modules.so", RTLD_NOW | RTLD_LOCAL);
    static auto function = library ? reinterpret_cast<Original>(
            dlsym(library, "sensor_util_post_downstream_event")) : nullptr;
    return function;
}
}
extern "C" int sensor_util_post_downstream_event(void *module, uint32_t identity,
        int eventType, void *data) {
    return oppo_camera_geometry_dispatch_sensor(module, identity, eventType, data, original());
}
