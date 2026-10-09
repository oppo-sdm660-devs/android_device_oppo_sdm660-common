/* SPDX-License-Identifier: BSD-3-Clause */
#include "CameraGeometry.h"
#include <cmath>
#include <cstring>
#include <deque>
#include <errno.h>
#include <map>
#include <mutex>

namespace {
std::mutex lock;
struct Stream {
    std::deque<oppo_camera_geometry_record> frames;
    bool requestedCpp = false;
};
std::map<uint32_t, Stream> streams;
struct SensorWindow { oppo_camera_sensor_window desired; bool applied = false; };
std::map<uint32_t, SensorWindow> sensors;
uint32_t word(const void *source, unsigned offset) {
    uint32_t value;
    memcpy(&value, static_cast<const uint8_t *>(source) + offset, sizeof(value));
    return value;
}
float real(const void *source, unsigned offset) {
    float value;
    memcpy(&value, static_cast<const uint8_t *>(source) + offset, sizeof(value));
    return value;
}
oppo_camera_geometry_record &frame(Stream &stream, uint32_t identity, uint32_t native) {
    for (auto &record : stream.frames) if (record.native_frame == native) return record;
    if (stream.frames.size() == 32) stream.frames.pop_front();
    stream.frames.push_back({});
    auto &record = stream.frames.back();
    record.identity = identity; record.native_frame = native;
    return record;
}
bool size(uint32_t width, uint32_t height) {
    return width && height && width <= 32768 && height <= 32768;
}
bool contains(uint32_t width, uint32_t height, const oppo_camera_geometry_rect &rect) {
    return size(width, height) && size(rect.width, rect.height) &&
            uint64_t(rect.left) + rect.width <= width && uint64_t(rect.top) + rect.height <= height;
}
}

extern "C" int oppo_camera_geometry_register(uint32_t identity) {
    if (!(identity >> 16) || !(identity & 0xffff)) return -EINVAL;
    std::lock_guard<std::mutex> guard(lock);
    if (streams.count(identity)) return -EEXIST;
    if (streams.size() >= 8) return -ENOSPC;
    streams.emplace(identity, Stream{});
    return 0;
}
extern "C" int oppo_camera_geometry_require_requested_cpp(uint32_t identity) {
    std::lock_guard<std::mutex> guard(lock);
    const auto stream = streams.find(identity);
    if (stream == streams.end()) return -ENOENT;
    stream->second.requestedCpp = true;
    return 0;
}
extern "C" int oppo_camera_geometry_has_requested_cpp(uint32_t identity) {
    std::lock_guard<std::mutex> guard(lock);
    const auto stream = streams.find(identity);
    return stream != streams.end() && stream->second.requestedCpp;
}
extern "C" int oppo_camera_geometry_configure_sensor(uint32_t session,
        const oppo_camera_sensor_window *desired) {
    if (!session || session > 0xffff || !desired ||
            !contains(desired->sensor_width, desired->sensor_height, desired->window) ||
            ((desired->window.left | desired->window.top |
              desired->window.width | desired->window.height) & 1)) return -EINVAL;
    std::lock_guard<std::mutex> guard(lock);
    if (sensors.count(session)) return -EEXIST;
    if (sensors.size() >= 8) return -ENOSPC;
    sensors.emplace(session, SensorWindow{*desired, false});
    return 0;
}
extern "C" void oppo_camera_geometry_clear_sensor(uint32_t session) {
    std::lock_guard<std::mutex> guard(lock);
    sensors.erase(session);
}
extern "C" int oppo_camera_geometry_get_sensor(uint32_t session,
        oppo_camera_sensor_window *actual) {
    if (!actual) return -EINVAL;
    *actual = {};
    std::lock_guard<std::mutex> guard(lock);
    const auto sensor = sensors.find(session);
    if (sensor == sensors.end()) return -ENOENT;
    if (!sensor->second.applied) return -EAGAIN;
    *actual = sensor->second.desired;
    return 0;
}
extern "C" int oppo_camera_geometry_dispatch_sensor(void *module, uint32_t identity,
        int eventType, void *data, int (*original)(void *, uint32_t, int, void *)) {
    if (!original) return 0;
    if (eventType != 0 || !data) return original(module, identity, eventType, data);
    const uint32_t session = identity >> 16;
    oppo_camera_sensor_window desired = {};
    {
        std::lock_guard<std::mutex> guard(lock);
        const auto sensor = sensors.find(session);
        if (sensor != sensors.end()) {
            sensor->second.applied = false;
            desired = sensor->second.desired;
        }
    }
    const bool matched = desired.sensor_width && word(data, 4) == desired.sensor_width &&
            word(data, 8) == desired.sensor_height && word(data, 12) == 0 &&
            word(data, 16) == desired.sensor_width - 1 && word(data, 20) == 0 &&
            word(data, 24) == desired.sensor_height - 1;
    if (matched) {
        const uint32_t crop[4] = {desired.window.left,
            desired.window.left + desired.window.width - 1, desired.window.top,
            desired.window.top + desired.window.height - 1};
        memcpy(static_cast<uint8_t *>(data) + 12, crop, sizeof(crop));
    }
    // The caller rereads this object to publish CAMIF metadata after dispatch.
    const int status = original(module, identity, eventType, data);
    if (matched && status) {
        std::lock_guard<std::mutex> guard(lock);
        const auto sensor = sensors.find(session);
        if (sensor != sensors.end()) sensor->second.applied = true;
    }
    return status;
}
extern "C" int oppo_camera_geometry_normalize_stats_sensor(uint32_t identity,
        const void *source, void *destination) {
    if (!source || !destination) return -EINVAL;
    oppo_camera_sensor_window desired = {};
    {
        std::lock_guard<std::mutex> guard(lock);
        const auto sensor = sensors.find(identity >> 16);
        if (sensor == sensors.end()) return 0;
        desired = sensor->second.desired;
    }
    if (word(source, 4) != desired.sensor_width ||
            word(source, 8) != desired.sensor_height ||
            word(source, 12) != desired.window.left ||
            word(source, 16) != desired.window.left + desired.window.width - 1 ||
            word(source, 20) != desired.window.top ||
            word(source, 24) != desired.window.top + desired.window.height - 1) return 0;
    // Stats consume CAMIF-local ROIs; IFACE/ISP keep the physical sensor origin.
    // This synchronous event precedes dispatch_sensor's successful-return marker.
    memcpy(destination, source, 528);
    const uint32_t crop[4] = {0, desired.window.width - 1, 0, desired.window.height - 1};
    memcpy(static_cast<uint8_t *>(destination) + 12, crop, sizeof(crop));
    return 1;
}
extern "C" void oppo_camera_geometry_unregister(uint32_t identity) {
    std::lock_guard<std::mutex> guard(lock);
    streams.erase(identity);
}
extern "C" int oppo_camera_geometry_get(uint32_t identity, uint32_t native,
        oppo_camera_geometry_record *output) {
    if (!output) return -EINVAL;
    std::lock_guard<std::mutex> guard(lock);
    const auto stream = streams.find(identity);
    if (stream == streams.end()) return -ENODEV;
    for (const auto &record : stream->second.frames) if (record.native_frame == native) {
        *output = record;
        return 0;
    }
    return -ENOENT;
}
extern "C" void oppo_camera_geometry_record_isp(uint32_t identity, uint32_t native, const void *zoom) {
    if (!zoom) return;
    std::lock_guard<std::mutex> guard(lock);
    const auto stream = streams.find(identity);
    if (stream == streams.end()) return;
    // Stock R11/R11s isp_zoom_params_t has an 80-byte stride. These fields
    // are read by the verified ISP48 ROI producer, after its real call.
    const auto fov = oppo_camera_geometry_rect{word(zoom, 0x40), word(zoom, 0x44),
        word(zoom, 0x48), word(zoom, 0x4c)};
    const uint32_t iw = word(zoom, 4), ih = word(zoom, 8);
    const uint32_t cw = word(zoom, 0x2c), ch = word(zoom, 0x30);
    const uint32_t sw = word(zoom, 0x34), sh = word(zoom, 0x38);
    const float scale = real(zoom, 0x3c);
    if (word(zoom, 0) != identity || !size(iw, ih) || !size(cw, ch) ||
            !contains(sw, sh, fov) || !std::isfinite(scale) || scale <= 0 || scale > 64) return;
    auto &record = frame(stream->second, identity, native);
    record.isp_width = iw; record.isp_height = ih;
    record.camif_width = cw; record.camif_height = ch;
    record.scaler_width = sw; record.scaler_height = sh;
    record.isp_scale = scale; record.isp_fov = fov;
    record.sensor_width = cw; record.sensor_height = ch;
    record.camif_window = {0, 0, cw, ch};
    const auto sensor = sensors.find(identity >> 16);
    if (sensor != sensors.end() && sensor->second.applied) {
        const auto &actual = sensor->second.desired;
        if (actual.window.width == cw && actual.window.height == ch) {
            record.sensor_width = actual.sensor_width;
            record.sensor_height = actual.sensor_height;
            record.camif_window = actual.window;
        }
    }
    record.valid |= OPPO_CAMERA_GEOMETRY_ISP;
}
extern "C" void oppo_camera_geometry_record_cpp(uint32_t identity, uint32_t native, const void *hardware) {
    if (!hardware) return;
    std::lock_guard<std::mutex> guard(lock);
    const auto stream = streams.find(identity);
    if (stream == streams.end()) return;
    // Stock CPP posts these exact values after cpp_params_calculate_crop().
    const auto window = oppo_camera_geometry_rect{word(hardware, 0x1620), word(hardware, 0x1624),
        word(hardware, 0x1628), word(hardware, 0x162c)};
    const uint32_t width = word(hardware, 0x1630), height = word(hardware, 0x1634);
    if (!contains(width, height, window)) return;
    auto &record = frame(stream->second, identity, native);
    record.cpp_input_width = width; record.cpp_input_height = height;
    record.cpp_window = window;
    record.valid |= OPPO_CAMERA_GEOMETRY_CPP;
}
