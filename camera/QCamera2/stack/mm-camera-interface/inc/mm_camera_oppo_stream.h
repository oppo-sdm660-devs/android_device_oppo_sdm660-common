/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MM_CAMERA_OPPO_STREAM_H
#define MM_CAMERA_OPPO_STREAM_H

#include "cam_intf.h"

/* R11/R11s C.19 ARM stream layout. */
#define MM_CAMERA_OPPO_STREAM_INFO_SIZE 0xc6f8U
#define MM_CAMERA_OPPO_STREAM_PARM_OFFSET 0x708U
#define MM_CAMERA_OPPO_STREAM_PARM_SIZE 0xbfbcU

int mm_camera_oppo_stream_encode(const cam_stream_info_t *native, void *wire);
int mm_camera_oppo_stream_decode(const void *wire, cam_stream_info_t *native);
int mm_camera_oppo_stream_parm_encode(const cam_stream_parm_buffer_t *native, void *wire);
void mm_camera_oppo_stream_parm_decode(const void *wire, cam_stream_parm_buffer_t *native);

#endif
