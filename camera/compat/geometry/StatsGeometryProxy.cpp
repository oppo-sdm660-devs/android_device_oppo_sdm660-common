/* SPDX-License-Identifier: BSD-3-Clause */
#include "CameraGeometry.h"
#include <cstring>
#include <dlfcn.h>
#include <map>
#include <mutex>

namespace {
using Event = int (*)(void *, void *);
using Init = int (*)(void *, uint32_t, void *);
using Deinit = void (*)(void *);
std::mutex portLock;
std::map<void *, Event> ports;
void *library() {
    static void *handle = dlopen("libmmcamera2_stats_modules.so", RTLD_NOW | RTLD_LOCAL);
    return handle;
}
template <class T> T original(const char *name) {
    return library() ? reinterpret_cast<T>(dlsym(library(), name)) : nullptr;
}
uint32_t word(const void *source, unsigned offset) {
    uint32_t value;
    memcpy(&value, static_cast<const uint8_t *>(source) + offset, sizeof(value));
    return value;
}
Event callback(void *port) {
    Event function;
    memcpy(&function, static_cast<uint8_t *>(port) + 0xe0, sizeof(function));
    return function;
}
void setCallback(void *port, Event function) {
    memcpy(static_cast<uint8_t *>(port) + 0xe0, &function, sizeof(function));
}
int event(void *port, void *incoming) {
    Event function;
    {
        std::lock_guard<std::mutex> guard(portLock);
        function = ports.at(port);
    }
    if (incoming && word(incoming, 0) == 2 && word(incoming, 8) == 1 &&
            word(incoming, 16) == 0) {
        static_assert(sizeof(void *) == 4, "Pinned stock MCT ABI is 32-bit");
        void *source;
        memcpy(&source, static_cast<const uint8_t *>(incoming) + 24, sizeof(source));
        alignas(8) uint8_t localSensor[528];
        if (oppo_camera_geometry_normalize_stats_sensor(word(incoming, 4),
                source, localSensor) == 1) {
            alignas(8) uint8_t localEvent[48];
            memcpy(localEvent, incoming, sizeof(localEvent));
            void *data = localSensor;
            memcpy(localEvent + 24, &data, sizeof(data));
            return function(port, localEvent);
        }
    }
    return function(port, incoming);
}
}

extern "C" int stats_port_init(void *port, uint32_t identity, void *subPorts) {
    static const auto function = original<Init>("stats_port_init");
    if (!function) return 0;
    const int status = function(port, identity, subPorts);
    if (status) {
        std::lock_guard<std::mutex> guard(portLock);
        ports[port] = callback(port);
        setCallback(port, event);
    }
    return status;
}
extern "C" void stats_port_deinit(void *port) {
    static const auto function = original<Deinit>("stats_port_deinit");
    {
        std::lock_guard<std::mutex> guard(portLock);
        const auto found = ports.find(port);
        if (found != ports.end()) {
            setCallback(port, found->second);
            ports.erase(found);
        }
    }
    if (function) function(port);
}
