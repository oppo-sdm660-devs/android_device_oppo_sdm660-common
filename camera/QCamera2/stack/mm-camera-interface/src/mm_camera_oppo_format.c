/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <stddef.h>

#include "mm_camera_oppo_format.h"

#define OPPO_FORMAT_SHIFT 4
#define OPPO_FORMAT_MAX 124

int32_t mm_camera_oppo_format_to_stock(cam_format_t native_format,
        int32_t *stock_format)
{
    if (stock_format == NULL) {
        return -EINVAL;
    }
    if (native_format == CAM_FORMAT_MAX) {
        *stock_format = OPPO_FORMAT_MAX;
    } else if ((int32_t)native_format >= CAM_FORMAT_JPEG &&
            native_format < CAM_FORMAT_BAYER_RAW_PLAIN16_10BPP_GBRG) {
        *stock_format = (int32_t)native_format;
    } else if (native_format >= CAM_FORMAT_JPEG_RAW_8BIT &&
            native_format < CAM_FORMAT_DEPTH16) {
        *stock_format = (int32_t)native_format - OPPO_FORMAT_SHIFT;
    } else {
        return -EINVAL;
    }
    return 0;
}

int32_t mm_camera_oppo_format_from_stock(int32_t stock_format,
        cam_format_t *native_format)
{
    if (native_format == NULL || stock_format < 0 ||
            stock_format > OPPO_FORMAT_MAX) {
        return -EINVAL;
    }
    if (stock_format == OPPO_FORMAT_MAX) {
        *native_format = CAM_FORMAT_MAX;
    } else if (stock_format < CAM_FORMAT_BAYER_RAW_PLAIN16_10BPP_GBRG) {
        *native_format = (cam_format_t)stock_format;
    } else {
        *native_format = (cam_format_t)(stock_format + OPPO_FORMAT_SHIFT);
    }
    return 0;
}
