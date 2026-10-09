/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MM_CAMERA_OPPO_CAPABILITY_H
#define MM_CAMERA_OPPO_CAPABILITY_H

#include <stddef.h>
#include <stdint.h>
#include "cam_intf.h"

/* R11/R11s Android 9 ARM capability layout. */
#define MM_CAMERA_OPPO_CAPABILITY_SIZE 27424U

#ifdef __cplusplus
extern "C" {
#endif
int mm_camera_oppo_capability_decode(const void *wire, size_t wire_size,
                                    cam_capability_t *native);
int mm_camera_oppo_capability_init(void *wire, size_t wire_size,
                                  uint32_t camera_index);
#ifdef __cplusplus
}
#endif
#endif
