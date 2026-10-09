/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef OPPO_CAMERA_GEOMETRY_H
#define OPPO_CAMERA_GEOMETRY_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct oppo_camera_geometry_rect { uint32_t left, top, width, height; };
struct oppo_camera_sensor_window {
    uint32_t sensor_width, sensor_height;
    struct oppo_camera_geometry_rect window;
};
struct oppo_camera_geometry_record {
    uint32_t identity, native_frame, valid;
    uint32_t isp_width, isp_height, camif_width, camif_height;
    uint32_t scaler_width, scaler_height;
    float isp_scale;
    struct oppo_camera_geometry_rect isp_fov;
    uint32_t cpp_input_width, cpp_input_height;
    struct oppo_camera_geometry_rect cpp_window;
    uint32_t sensor_width, sensor_height;
    struct oppo_camera_geometry_rect camif_window;
};
enum { OPPO_CAMERA_GEOMETRY_ISP = 1, OPPO_CAMERA_GEOMETRY_CPP = 2 };
int oppo_camera_geometry_register(uint32_t identity);
int oppo_camera_geometry_require_requested_cpp(uint32_t identity);
int oppo_camera_geometry_has_requested_cpp(uint32_t identity);
void oppo_camera_geometry_unregister(uint32_t identity);
int oppo_camera_geometry_get(uint32_t identity, uint32_t native_frame,
        struct oppo_camera_geometry_record *record);
void oppo_camera_geometry_record_isp(uint32_t identity, uint32_t native_frame, const void *zoom);
void oppo_camera_geometry_record_cpp(uint32_t identity, uint32_t native_frame, const void *hardware);
int oppo_camera_geometry_configure_sensor(uint32_t session,
        const struct oppo_camera_sensor_window *desired);
void oppo_camera_geometry_clear_sensor(uint32_t session);
int oppo_camera_geometry_get_sensor(uint32_t session, struct oppo_camera_sensor_window *actual);
int oppo_camera_geometry_dispatch_sensor(void *module, uint32_t identity, int event_type,
        void *data, int (*original)(void *, uint32_t, int, void *));
int oppo_camera_geometry_normalize_stats_sensor(uint32_t identity, const void *source,
        void *destination);
#ifdef __cplusplus
}
#endif
#endif
