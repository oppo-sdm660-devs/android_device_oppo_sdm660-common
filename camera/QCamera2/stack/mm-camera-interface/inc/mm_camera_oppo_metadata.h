/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MM_CAMERA_OPPO_METADATA_H
#define MM_CAMERA_OPPO_METADATA_H

#include <stddef.h>
#include "cam_intf.h"

/* Stock metadata includes private tails beyond the accessor table at 0x7ab48. */
#define MM_CAMERA_OPPO_METADATA_SIZE 0x11dc88U
#define MM_CAMERA_OPPO_PARAMETER_MAX 287U

typedef struct {
    int fd;
    size_t size;
    void *data;
} mm_camera_oppo_buffer_t;

#ifdef __cplusplus
extern "C" {
#endif
int mm_camera_oppo_buffer_alloc(mm_camera_oppo_buffer_t *buffer, size_t size);
void mm_camera_oppo_buffer_release(mm_camera_oppo_buffer_t *buffer);
int mm_camera_oppo_metadata_encode(const metadata_buffer_t *native, void *wire);
void mm_camera_oppo_metadata_decode(const void *wire, metadata_buffer_t *native);
#ifdef __cplusplus
}
#endif
#endif
