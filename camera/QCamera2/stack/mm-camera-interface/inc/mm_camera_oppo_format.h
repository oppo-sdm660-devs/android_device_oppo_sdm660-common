/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MM_CAMERA_OPPO_FORMAT_H
#define MM_CAMERA_OPPO_FORMAT_H

#include "cam_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Stock omits four plain16 RAW and three depth formats. CAM_FORMAT_MAX
 * remains valid for metadata; unsupported formats return -EINVAL. */
int32_t mm_camera_oppo_format_to_stock(cam_format_t native_format,
        int32_t *stock_format);
int32_t mm_camera_oppo_format_from_stock(int32_t stock_format,
        cam_format_t *native_format);

#ifdef __cplusplus
}
#endif

#endif
