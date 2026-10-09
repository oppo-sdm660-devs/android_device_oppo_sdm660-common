/* SPDX-License-Identifier: BSD-3-Clause */
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include "mm_camera_oppo_dual.h"

_Static_assert(sizeof(cam_dual_camera_bundle_info_t) == 40, "Native dual bundle");
_Static_assert(sizeof(cam_dual_camera_cmd_info_t) == 44, "Native dual command");
_Static_assert(CAM_PRIV_DUAL_CAM_CMD == 0x0800001c, "Stock dual private control");
_Static_assert(CAM_MAPPING_BUF_TYPE_DUAL_CAM_CMD_BUF == 3, "Stock dual mapping");
_Static_assert(CAM_DUAL_CAMERA_BUNDLE_INFO == 0 &&
        CAM_DUAL_CAMERA_LOW_POWER_MODE == 1 && CAM_DUAL_CAMERA_MASTER_INFO == 2 &&
        CAM_DUAL_CAMERA_DEFER_INFO == 3 && CAM_DUAL_CAMERA_FALLBACK_INFO == 4,
        "Stock dual command IDs");

static void put32(unsigned char *wire, size_t offset, uint32_t value)
{
    /* Both the HAL and stock backend are little-endian ARM. */
    wire[offset] = value;
    wire[offset + 1] = value >> 8;
    wire[offset + 2] = value >> 16;
    wire[offset + 3] = value >> 24;
}

int mm_camera_oppo_dual_encode(const cam_dual_camera_cmd_info_t *native,
        void *wire, size_t wire_size)
{
    if (native == NULL || wire == NULL || wire_size != MM_CAMERA_OPPO_DUAL_CMD_SIZE)
        return -EINVAL;

    /* Stage before publishing: a rejected command preserves the mapped buffer. */
    unsigned char encoded[MM_CAMERA_OPPO_DUAL_CMD_SIZE] = {0};
    put32(encoded, 0, native->cmd_type);
    switch (native->cmd_type) {
    case CAM_DUAL_CAMERA_BUNDLE_INFO: {
        const cam_dual_camera_bundle_info_t *b = &native->bundle_info;
        if ((b->sync_control != CAM_SYNC_RELATED_SENSORS_ON &&
                b->sync_control != CAM_SYNC_RELATED_SENSORS_OFF) ||
                (unsigned)b->sync_mechanism > CAM_SYNC_HYBRID_SYNC ||
                (b->type != CAM_TYPE_MAIN && b->type != CAM_TYPE_AUX) ||
                (unsigned)b->mode > CAM_MODE_SECONDARY ||
                (unsigned)b->sync_3a_config.sync_mode_stats > CAM_3A_SYNC_ALGO_CTRL ||
                (unsigned)b->sync_3a_config.sync_mode_af > CAM_3A_SYNC_ALGO_CTRL ||
                (unsigned)b->perf_mode > CAM_PERF_STATS_FPS_CONTROL ||
                b->related_sensor_session_id == 0 ||
                b->related_sensor_session_id > UINT16_MAX || b->hal_lpm_control > 1)
            return -EINVAL;
        /* Stock has one scalar shared by AEC/AWB/AF, no independent AF mode. */
        if (b->sync_3a_config.sync_mode_stats != b->sync_3a_config.sync_mode_af ||
                b->hal_lpm_control != 0 ||
                (b->cam_role != CAM_ROLE_DEFAULT && b->cam_role != CAM_ROLE_WIDE &&
                 b->cam_role != CAM_ROLE_TELE))
            return -ENOTSUP;
        put32(encoded, 4, b->sync_control);
        put32(encoded, 8, b->sync_mechanism);
        put32(encoded, 12, b->type);
        put32(encoded, 16, b->mode);
        put32(encoded, 20, b->sync_3a_config.sync_mode_stats);
        put32(encoded, 24, b->cam_role);
        /* MCT mutates this word to (session << 16)|0xf; always rebuild it. */
        put32(encoded, 28, b->related_sensor_session_id);
        put32(encoded, 32, b->perf_mode);
        /* Nonzero HAL-owned LPM has no stock field and is rejected above. */
        break;
    }
    case CAM_DUAL_CAMERA_LOW_POWER_MODE:
        if ((unsigned)native->value.perf_mode > CAM_PERF_STATS_FPS_CONTROL ||
                native->value.enable > 1 || native->value.priority > 1)
            return -EINVAL;
        put32(encoded, 4, native->value.perf_mode);
        encoded[8] = native->value.enable;
        encoded[9] = native->value.priority;
        break;
    case CAM_DUAL_CAMERA_MASTER_INFO:
        if ((unsigned)native->mode.mode > CAM_MODE_SECONDARY)
            return -EINVAL;
        put32(encoded, 4, native->mode.mode);
        break;
    case CAM_DUAL_CAMERA_DEFER_INFO:
        if ((unsigned)native->defer_cmd > CAM_DEFER_FLUSH)
            return -EINVAL;
        put32(encoded, 4, native->defer_cmd);
        break;
    case CAM_DUAL_CAMERA_FALLBACK_INFO:
        if ((unsigned)native->fallback.fallback > CAM_WIDE_FALLBACK)
            return -EINVAL;
        put32(encoded, 4, native->fallback.fallback);
        break;
    default:
        return -EINVAL;
    }
    memcpy(wire, encoded, sizeof(encoded));
    return 0;
}
