/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MM_CAMERA_OPPO_DUAL_H
#define MM_CAMERA_OPPO_DUAL_H

#include <stddef.h>
#include "cam_intf.h"

/* R11/R11s stock HAL/MCT: command word followed by a 32-byte union. */
#define MM_CAMERA_OPPO_DUAL_CMD_SIZE 36U

#ifdef __cplusplus
extern "C" {
#endif
int mm_camera_oppo_dual_encode(const cam_dual_camera_cmd_info_t *native,
        void *wire, size_t wire_size);
#ifdef __cplusplus
}
#endif
#endif
