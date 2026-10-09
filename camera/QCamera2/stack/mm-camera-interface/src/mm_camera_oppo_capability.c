/* SPDX-License-Identifier: BSD-3-Clause */
#include "mm_camera_oppo_capability.h"
#include "mm_camera_oppo_format.h"

#include <errno.h>
#include <math.h>
#include <string.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* Offsets use the R11/R11s 32-bit daemon layout. */
static uint32_t wire_u32(const uint8_t *wire, size_t offset)
{
    uint32_t value;
    memcpy(&value, wire + offset, sizeof(value));
    return value;
}

static int copy_table(const uint8_t *wire, size_t count_offset,
                      size_t count_size, size_t table_offset,
                      size_t stock_capacity, size_t element_size,
                      void *destination, size_t capacity, size_t *count)
{
    if (element_size == 0 || (count_size != 1 && count_size != 4) ||
            count_offset > MM_CAMERA_OPPO_CAPABILITY_SIZE - count_size)
        return -EINVAL;
    uint32_t entries = count_size == 1 ? wire[count_offset] :
            wire_u32(wire, count_offset);
    if (entries > stock_capacity ||
            table_offset > MM_CAMERA_OPPO_CAPABILITY_SIZE ||
            stock_capacity > (MM_CAMERA_OPPO_CAPABILITY_SIZE - table_offset) /
            element_size)
        return -EINVAL;
    *count = entries < capacity ? entries : capacity;
    memcpy(destination, wire + table_offset, *count * element_size);
    return 0;
}

static int dimensions_valid(const cam_dimension_t *table, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (table[i].width <= 0 || table[i].height <= 0 ||
                table[i].width > 32768 || table[i].height > 32768)
            return 0;
    }
    return 1;
}

static int decode_fps_ranges(const uint8_t *wire, cam_capability_t *native)
{
    _Static_assert(sizeof(cam_fps_range_t) == 16, "Stock FPS record size");
    const uint32_t stock_count = wire_u32(wire, 0x2d0);
    if (stock_count > 60)
        return -EINVAL;
    for (size_t i = 0; i < stock_count; i++) {
        cam_fps_range_t range;
        memcpy(&range, wire + 0x2d4 + i * 16, sizeof(range));
        if (!isfinite(range.min_fps) || !isfinite(range.max_fps) ||
                !isfinite(range.video_min_fps) || !isfinite(range.video_max_fps) ||
                range.min_fps <= 0 || range.max_fps < range.min_fps ||
                range.video_min_fps < 0 || range.video_max_fps < range.video_min_fps)
            return -EINVAL;
        if (range.max_fps <= 30 && range.video_max_fps <= 30 &&
                native->fps_ranges_tbl_cnt < ARRAY_SIZE(native->fps_ranges_tbl))
            native->fps_ranges_tbl[native->fps_ranges_tbl_cnt++] = range;
    }
    return 0;
}

/* Stock padding has no usage member. */
static void decode_padding(const uint8_t *wire, size_t offset,
                           cam_padding_info_t *padding)
{
    padding->width_padding = wire_u32(wire, offset);
    padding->height_padding = wire_u32(wire, offset + 4);
    padding->plane_padding = wire_u32(wire, offset + 8);
    padding->min_stride = wire_u32(wire, offset + 12);
    padding->min_scanline = wire_u32(wire, offset + 16);
    padding->offset_info.offset_x = wire_u32(wire, offset + 20);
    padding->offset_info.offset_y = wire_u32(wire, offset + 24);
    padding->usage = 0;
}

int mm_camera_oppo_capability_init(void *wire, size_t wire_size,
                                  uint32_t camera_index)
{
    if (wire == NULL || wire_size != MM_CAMERA_OPPO_CAPABILITY_SIZE)
        return -EINVAL;
    memset(wire, 0, wire_size);
    memcpy((uint8_t *)wire + 0x6ad8, &camera_index, sizeof(camera_index));
    return 0;
}

int mm_camera_oppo_capability_decode(const void *buffer, size_t wire_size,
                                    cam_capability_t *native)
{
    if (buffer == NULL || native == NULL ||
            wire_size != MM_CAMERA_OPPO_CAPABILITY_SIZE)
        return -EINVAL;
    const uint8_t *wire = buffer;
    memset(native, 0, sizeof(*native));

#define FIELD(member, offset, bytes) \
    do { \
        _Static_assert(sizeof(native->member) == (bytes), "Capability field size: " #member); \
        _Static_assert((offset) + (bytes) <= MM_CAMERA_OPPO_CAPABILITY_SIZE, \
                "Capability field bounds: " #member); \
        memcpy(&native->member, wire + (offset), (bytes)); \
    } while (0)
#define TABLE(member, count_member, count_offset, count_size, table_offset, stock_capacity, element_bytes) \
    do { \
        _Static_assert(sizeof(native->member[0]) == (element_bytes), \
                "Capability table element: " #member); \
        _Static_assert((table_offset) + (stock_capacity) * (element_bytes) <= \
                MM_CAMERA_OPPO_CAPABILITY_SIZE, "Capability table bounds: " #member); \
        size_t entries; \
        int status = copy_table(wire, (count_offset), (count_size), (table_offset), \
                (stock_capacity), sizeof(native->member[0]), native->member, \
                ARRAY_SIZE(native->member), &entries); \
        if (status != 0) \
            return status; \
        native->count_member = entries; \
    } while (0)

    FIELD(version, 0x0, 4);
    FIELD(position, 0x4, 4);
    FIELD(exposure_compensation_min, 0x750, 4);
    FIELD(exposure_compensation_max, 0x754, 4);
    FIELD(exposure_compensation_default, 0x758, 4);
    FIELD(exposure_compensation_step, 0x75c, 4);
    FIELD(exp_compensation_step, 0x760, 8);
    FIELD(modes_supported, 0xb30, 4);
    FIELD(sensor_mount_angle, 0xb34, 4);
    FIELD(focal_length, 0xb38, 4);
    FIELD(hor_view_angle, 0xb3c, 4);
    FIELD(ver_view_angle, 0xb40, 4);
    FIELD(q3a_version, 0x4280, 8);
    FIELD(auto_wb_lock_supported, 0x429c, 1);
    FIELD(zoom_supported, 0x429d, 1);
    FIELD(smooth_zoom_supported, 0x429e, 1);
    FIELD(auto_exposure_lock_supported, 0x429f, 1);
    FIELD(video_snapshot_supported, 0x42a0, 1);
    FIELD(max_num_roi, 0x42a1, 1);
    FIELD(max_num_focus_areas, 0x42a2, 1);
    FIELD(max_num_metering_areas, 0x42a3, 1);
    FIELD(max_zoom_step, 0x42a4, 1);
    FIELD(min_num_pp_bufs, 0x4344, 4);
    FIELD(min_focus_distance, 0x434c, 4);
    FIELD(hyper_focal_distance, 0x4350, 4);
    FIELD(focal_lengths, 0x4354, 4);
    FIELD(focal_lengths_count, 0x4358, 1);
    FIELD(apertures, 0x435c, 4);
    FIELD(apertures_count, 0x4360, 1);
    FIELD(filter_densities, 0x4364, 4);
    FIELD(filter_densities_count, 0x4368, 1);
    FIELD(optical_stab_modes, 0x436c, 8);
    FIELD(optical_stab_modes_count, 0x4374, 1);
    FIELD(lens_shading_map_size, 0x4378, 8);
    FIELD(lens_position, 0x46e8, 12);
    FIELD(exposure_time_range, 0x46f8, 16);
    FIELD(max_frame_duration, 0x4708, 8);
    FIELD(color_arrangement, 0x4710, 4);
    FIELD(num_color_channels, 0x4714, 1);
    FIELD(gradient_S, 0x4718, 8);
    FIELD(offset_S, 0x4720, 8);
    FIELD(gradient_O, 0x4728, 8);
    FIELD(offset_O, 0x4730, 8);
    FIELD(sensor_physical_size, 0x4738, 8);
    FIELD(pixel_array_size, 0x4740, 8);
    FIELD(active_array_size, 0x4748, 16);
    FIELD(white_level, 0x4758, 4);
    FIELD(black_level_pattern, 0x475c, 16);
    FIELD(flash_charge_duration, 0x4770, 8);
    FIELD(max_tone_map_curve_points, 0x47b4, 4);
    FIELD(histogram_supported, 0x49b1, 1);
    FIELD(histogram_size, 0x49b4, 4);
    FIELD(max_histogram_count, 0x49b8, 4);
    FIELD(sharpness_map_size, 0x49bc, 8);
    FIELD(max_sharpness_map_value, 0x49c4, 4);
    FIELD(sensitivity_range, 0x49d4, 8);
    FIELD(max_analog_sensitivity, 0x49dc, 4);
    FIELD(isp_sensitivity_range, 0x49e0, 8);
    FIELD(flash_available, 0x4a74, 1);
    FIELD(base_gain_factor, 0x4a78, 8);
    FIELD(focus_dist_calibrated, 0x4b38, 1);
    FIELD(reference_illuminant1, 0x4d40, 4);
    FIELD(reference_illuminant2, 0x4d44, 4);
    FIELD(forward_matrix1, 0x5108, 72);
    FIELD(forward_matrix2, 0x5150, 72);
    FIELD(color_transform1, 0x5198, 72);
    FIELD(color_transform2, 0x51e0, 72);
    FIELD(calibration_transform1, 0x5228, 72);
    FIELD(calibration_transform2, 0x5270, 72);
    FIELD(sensor_type.sens_type, 0x52c4, 4);
    FIELD(aberration_modes, 0x52cc, 12);
    FIELD(aberration_modes_count, 0x52d8, 4);
    FIELD(isTimestampCalibrated, 0x52dc, 1);
    FIELD(max_viewfinder_size, 0x52e0, 8);
    FIELD(no_per_frame_control_support, 0x5384, 1);
    FIELD(buf_alignment, 0x53bc, 4);
    FIELD(min_stride, 0x53c0, 4);
    FIELD(min_scanline, 0x53c4, 4);
    FIELD(max_pixel_bandwidth, 0x5428, 8);
    FIELD(optical_black_regions, 0x5430, 80);
    FIELD(optical_black_region_count, 0x5480, 1);
    FIELD(min_wb_cct, 0x6f8, 4);
    FIELD(max_wb_cct, 0x6fc, 4);
    FIELD(min_wb_gain, 0x700, 4);
    FIELD(max_wb_gain, 0x704, 4);
    FIELD(min_focus_pos, 0x730, 16);
    FIELD(max_focus_pos, 0x740, 16);
    FIELD(max_downscale_factor, 0x3cc0, 1);

    TABLE(supported_flash_modes, supported_flash_modes_cnt, 0x68, 4, 0x6c, 5, 4);
    TABLE(zoom_ratio_tbl, zoom_ratio_tbl_cnt, 0x80, 4, 0x84, 101, 4);
    native->supported_effects_cnt = 1;
    native->supported_effects[0] = CAM_EFFECT_MODE_OFF;
    native->supported_scene_modes_cnt = 1;
    native->supported_scene_modes[0] = CAM_SCENE_MODE_OFF;
    TABLE(supported_aec_modes, supported_aec_modes_cnt, 0x2b0, 4, 0x2b4, 7, 4);
    TABLE(supported_antibandings, supported_antibandings_cnt, 0x694, 4, 0x698, 6, 4);
    TABLE(supported_white_balances, supported_white_balances_cnt, 0x6b0, 4, 0x6b4, 11, 4);
    native->supported_sensor_hdr_types_cnt = 1;
    native->supported_sensor_hdr_types[0] = CAM_SENSOR_HDR_OFF;
    TABLE(supported_focus_modes, supported_focus_modes_cnt, 0x708, 4, 0x70c, 9, 4);
    TABLE(picture_sizes_tbl, picture_sizes_tbl_cnt, 0x76c, 4, 0x770, 60, 8);
    TABLE(preview_sizes_tbl, preview_sizes_tbl_cnt, 0xb48, 4, 0xb4c, 60, 8);
    TABLE(video_sizes_tbl, video_sizes_tbl_cnt, 0xd2c, 4, 0xd30, 60, 8);
    TABLE(livesnapshot_sizes_tbl, livesnapshot_sizes_tbl_cnt, 0xf10, 4, 0xf14, 60, 8);
    TABLE(raw_dim, supported_raw_dim_cnt, 0x3cc4, 4, 0x3cc8, 60, 8);
    TABLE(supported_focus_algos, supported_focus_algos_cnt, 0x4288, 4, 0x428c, 4, 4);
    TABLE(supported_firing_levels, supported_flash_firing_level_cnt, 0x4778, 4, 0x477c, 11, 4);
    TABLE(supported_ae_modes, supported_ae_modes_cnt, 0x49c8, 4, 0x49cc, 2, 4);
    TABLE(scale_picture_sizes, scale_picture_sizes_cnt, 0x4a30, 4, 0x4a34, 8, 8);
    native->supported_test_pattern_modes_cnt = 1;
    native->supported_test_pattern_modes[0] = CAM_TEST_PATTERN_OFF;
    native->supported_is_types_cnt = 1;
    native->supported_is_types[0] = IS_TYPE_NONE;
    native->video_stablization_supported = 0;
    native->auto_hdr_supported = 0;

    if (decode_fps_ranges(wire, native) != 0 ||
            mm_camera_oppo_format_from_stock(wire_u32(wire, 0x4348),
                &native->rdi_mode_stream_fmt) != 0 ||
            mm_camera_oppo_format_from_stock(wire_u32(wire, 0x52c8),
                &native->sensor_type.native_format) != 0)
        return -EINVAL;

    memcpy(native->picture_min_duration, wire + 0x950,
            native->picture_sizes_tbl_cnt * sizeof(int64_t));
    memcpy(native->stall_durations, wire + 0x4b60,
            native->picture_sizes_tbl_cnt * sizeof(int64_t));
    memcpy(native->jpeg_stall_durations, wire + 0x4d48,
            native->picture_sizes_tbl_cnt * sizeof(int64_t));
    memcpy(native->raw_min_duration, wire + 0x40a0,
            native->supported_raw_dim_cnt * sizeof(int64_t));
    memcpy(native->raw16_stall_durations, wire + 0x4f28,
            native->supported_raw_dim_cnt * sizeof(int64_t));

    decode_padding(wire, 0x4328, &native->padding_info);
    memcpy(native->flash_dev_name, wire + 0x53c8,
            sizeof(native->flash_dev_name));
    native->flash_dev_name[sizeof(native->flash_dev_name) - 1] = 0;
    memcpy(native->eeprom_version_info, wire + 0x5408,
            sizeof(native->eeprom_version_info));
    native->camera_index = wire_u32(wire, 0x6ad8);

    /* Daemon pointers and private features are not exported. */
    native->main_cam_cap = NULL;
    native->aux_cam_cap = NULL;
    native->qcom_supported_feature_mask = 0;
    native->related_cam_calibration.dc_otp_params = NULL;
    native->related_cam_calibration.dc_otp_size = 0;
    if (native->optical_black_region_count > MAX_OPTICAL_BLACK_REGIONS ||
            native->apertures_count > CAM_APERTURES_MAX ||
            native->filter_densities_count > CAM_FILTER_DENSITIES_MAX ||
            native->focal_lengths_count > CAM_FOCAL_LENGTHS_MAX ||
            native->optical_stab_modes_count > CAM_OPT_STAB_MAX ||
            native->aberration_modes_count > CAM_COLOR_CORRECTION_ABERRATION_MAX ||
            native->num_color_channels == 0 || native->num_color_channels > 4 ||
            native->max_downscale_factor == 0)
        return -EINVAL;
    if (native->picture_sizes_tbl_cnt == 0 || native->preview_sizes_tbl_cnt == 0 ||
            native->fps_ranges_tbl_cnt == 0 || native->zoom_ratio_tbl_cnt == 0 ||
            !dimensions_valid(native->picture_sizes_tbl, native->picture_sizes_tbl_cnt) ||
            !dimensions_valid(native->preview_sizes_tbl, native->preview_sizes_tbl_cnt) ||
            !dimensions_valid(native->video_sizes_tbl, native->video_sizes_tbl_cnt) ||
            !dimensions_valid(native->raw_dim, native->supported_raw_dim_cnt) ||
            !dimensions_valid(&native->pixel_array_size, 1) ||
            native->active_array_size.left < 0 || native->active_array_size.top < 0 ||
            native->active_array_size.width <= 0 || native->active_array_size.height <= 0 ||
            (int64_t)native->active_array_size.left + native->active_array_size.width >
                    native->pixel_array_size.width ||
            (int64_t)native->active_array_size.top + native->active_array_size.height >
                    native->pixel_array_size.height)
        return -EINVAL;
    for (size_t i = 0; i < native->zoom_ratio_tbl_cnt; i++) {
        if (native->zoom_ratio_tbl[i] == 0 ||
                (i != 0 && native->zoom_ratio_tbl[i] < native->zoom_ratio_tbl[i - 1]))
            return -EINVAL;
    }
    if (native->position > CAM_POSITION_FRONT_AUX ||
            native->sensor_mount_angle >= 360 || native->sensor_mount_angle % 90 != 0 ||
            native->sensor_type.sens_type > CAM_SENSOR_MONO)
        return -EINVAL;
#undef TABLE
#undef FIELD
    return 0;
}
