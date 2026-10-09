/* SPDX-License-Identifier: BSD-3-Clause */
#include <errno.h>
#include <stddef.h>
#include <string.h>
#include "mm_camera_oppo_metadata.h"
#include "mm_camera_oppo_format.h"
#include "mm_camera_oppo_stream.h"

/* OPPO shares only the 440-byte prefix; PP, parameter and tail layouts differ. */
_Static_assert(offsetof(cam_stream_info_t, pp_config) == 440, "ARM stream prefix");
_Static_assert(sizeof(cam_stream_buf_plane_info_t) == 392, "ARM plane layout");
_Static_assert(sizeof(cam_pp_offline_src_config_t) == 412, "ARM offline source");
_Static_assert(offsetof(cam_reprocess_param, crop_rect) + 4 == 49056,
        "ARM reprocess crop");
_Static_assert(offsetof(cam_reprocess_param, is_uv_subsampled) + 4 == 49072,
        "ARM reprocess UV flag");
_Static_assert(MM_CAMERA_OPPO_STREAM_PARM_OFFSET + MM_CAMERA_OPPO_STREAM_PARM_SIZE
        == 0xc6c4, "Stock stream tail boundary");

static void put(void *wire, size_t offset, const void *value, size_t size)
{
    memcpy((unsigned char *)wire + offset, value, size);
}

#define PUT(wire, offset, value) put(wire, offset, &(value), sizeof(value))

/* Private PP blocks remain zero. */
static void encode_pp(const cam_pp_feature_config_t *native, void *wire)
{
    cam_feature_mask_t mask = native->feature_mask &
            (CAM_QCOM_FEATURE_CROP | CAM_QCOM_FEATURE_ROTATION |
             CAM_QCOM_FEATURE_FLIP | CAM_QCOM_FEATURE_SCALE);
    PUT(wire, 0, mask);
    PUT(wire, 16, native->input_crop);
    PUT(wire, 32, native->rotation);
    PUT(wire, 36, native->flip);
    PUT(wire, 440, native->scale_param);
    PUT(wire, 464, native->cur_reproc_count);
    PUT(wire, 465, native->total_reproc_count);
}

int mm_camera_oppo_stream_encode(const cam_stream_info_t *native, void *wire)
{
    if (native == NULL || wire == NULL)
        return -EINVAL;
    if (native->is_secure || native->aux_str_info != NULL)
        return -ENOTSUP;
    memset(wire, 0, MM_CAMERA_OPPO_STREAM_INFO_SIZE);
    memcpy(wire, native, 440);
    int32_t format;
    int result = mm_camera_oppo_format_to_stock(native->fmt, &format);
    if (result != 0)
        return result;
    PUT(wire, 8, format);
    if (native->stream_type == CAM_STREAM_TYPE_METADATA) {
        cam_dimension_t dim = { MM_CAMERA_OPPO_METADATA_SIZE, 1 };
        PUT(wire, 12, dim);
    }
    encode_pp(&native->pp_config, (unsigned char *)wire + 0x1b8);
    if (native->stream_type == CAM_STREAM_TYPE_OFFLINE_PROC) {
        const cam_stream_reproc_config_t *config = &native->reprocess_config;
        PUT(wire, 0x390, config->pp_type);
        if (config->pp_type == CAM_OFFLINE_REPROCESS_TYPE) {
            PUT(wire, 0x394, config->offline);
            result = mm_camera_oppo_format_to_stock(config->offline.input_fmt, &format);
            if (result != 0)
                return result;
            PUT(wire, 0x394, format);
            if (config->offline.input_type == CAM_STREAM_TYPE_METADATA) {
                cam_dimension_t dim = { MM_CAMERA_OPPO_METADATA_SIZE, 1 };
                PUT(wire, 0x398, dim);
            }
        } else {
            PUT(wire, 0x394, config->online);
        }
        encode_pp(&config->pp_feature_config, (unsigned char *)wire + 0x530);
    }
    PUT(wire, 0xc6c4, native->dis_enable);
    PUT(wire, 0xc6c8, native->is_type);
    PUT(wire, 0xc6cc, native->is_secure);
    PUT(wire, 0xc6d0, native->perf_mode);
    PUT(wire, 0xc6d4, native->noFrameExpected);
    PUT(wire, 0xc6d8, native->dt);
    PUT(wire, 0xc6dc, native->vc);
    PUT(wire, 0xc6e0, native->sub_format_type);
    /* 0xc6e4 is the unused stock auxiliary-stream pointer. */
    PUT(wire, 0xc6e8, native->cache_ops);
    PUT(wire, 0xc6ec, native->cam_type);
    PUT(wire, 0xc6f0, native->bStreamSyncCbNeeded);
    PUT(wire, 0xc6f4, native->bNoBundling);
    return 0;
}

int mm_camera_oppo_stream_decode(const void *wire, cam_stream_info_t *native)
{
    if (wire == NULL || native == NULL)
        return -EINVAL;
    cam_format_t format;
    int32_t stock_format;
    memcpy(&stock_format, (const unsigned char *)wire + 8, sizeof(stock_format));
    int result = mm_camera_oppo_format_from_stock(stock_format, &format);
    if (result != 0)
        return result;
    /* Metadata dimensions describe the native shadow, not the stock buffer. */
    cam_dimension_t dim = native->dim;
    memcpy(native, wire, 440);
    native->fmt = format;
    if (native->stream_type == CAM_STREAM_TYPE_METADATA)
        native->dim = dim;
    if (native->buf_planes.plane_info.num_planes > VIDEO_MAX_PLANES ||
            native->num_bufs > CAM_MAX_NUM_BUFS_PER_STREAM ||
            native->buf_cnt > CAM_MAX_NUM_BUFS_PER_STREAM)
        return -EINVAL;
    return 0;
}

/* Stock parameters use metadata IDs and an eight-byte larger union. */
int mm_camera_oppo_stream_parm_encode(const cam_stream_parm_buffer_t *native, void *wire)
{
    if (native == NULL || wire == NULL)
        return -EINVAL;
    memset(wire, 0, MM_CAMERA_OPPO_STREAM_PARM_SIZE);
    switch (native->type) {
    case CAM_STREAM_PARAM_TYPE_DO_REPROCESS: {
        uint32_t type = 0x62;
        PUT(wire, 0, type);
        PUT(wire, 4, native->reprocess.buf_index);
        PUT(wire, 8, native->reprocess.frame_idx);
        PUT(wire, 12, native->reprocess.ret_val);
        PUT(wire, 16, native->reprocess.meta_present);
        PUT(wire, 20, native->reprocess.meta_stream_handle);
        PUT(wire, 24, native->reprocess.meta_buf_index);
        PUT(wire, 28, native->reprocess.is_offline_meta_bypass);
        /* Payload size is in bytes, although private_data is an int32_t array. */
        put(wire, 32, native->reprocess.private_data,
                MAX_METADATA_PRIVATE_PAYLOAD_SIZE_IN_BYTES);
        PUT(wire, 49056, native->reprocess.crop_rect);
        PUT(wire, 49072, native->reprocess.is_uv_subsampled);
        /* HAL1 CPP configuration/crop words at 49076/49080 remain zero. */
        return 0;
    }
    case CAM_STREAM_PARAM_TYPE_SET_BUNDLE_INFO: {
        if (native->bundleInfo.num_of_streams > MAX_STREAM_NUM_IN_BUNDLE)
            return -EINVAL;
        uint32_t type = 0x63;
        PUT(wire, 0, type);
        PUT(wire, 4, native->bundleInfo);
        return 0;
    }
    case CAM_STREAM_PARAM_TYPE_SET_FLIP: {
        uint32_t type = 0x64;
        PUT(wire, 0, type);
        PUT(wire, 4, native->flipInfo);
        return 0;
    }
    case CAM_STREAM_PARAM_TYPE_GET_OUTPUT_CROP: {
        uint32_t type = 0x65;
        PUT(wire, 0, type);
        return 0;
    }
    case CAM_STREAM_PARAM_TYPE_GET_IMG_PROP: {
        uint32_t type = 0xa7;
        PUT(wire, 0, type);
        return 0;
    }
    case CAM_STREAM_PARAM_TYPE_REQUEST_OPS_MODE: {
        uint32_t type = 0xcd;
        PUT(wire, 0, type);
        PUT(wire, 4, native->perf_mode);
        return 0;
    }
    case CAM_STREAM_PARAM_TYPE_REQUEST_FRAMES: {
        uint32_t type = 0xcc;
        PUT(wire, 0, type);
        PUT(wire, 4, native->frameRequest);
        return 0;
    }
    case CAM_STREAM_PARAM_TYPE_FRAME_SKIP: {
        uint32_t type = 48;
        PUT(wire, 0, type);
        PUT(wire, 4, native->skipPattern);
        return 0;
    }
    default:
        return -ENOTSUP;
    }
}

void mm_camera_oppo_stream_parm_decode(const void *wire, cam_stream_parm_buffer_t *native)
{
    const unsigned char *stock = wire;
    if (native->type == CAM_STREAM_PARAM_TYPE_DO_REPROCESS)
        memcpy(&native->reprocess.ret_val, stock + 12, sizeof(native->reprocess.ret_val));
    else if (native->type == CAM_STREAM_PARAM_TYPE_GET_OUTPUT_CROP) {
        memcpy(&native->outputCrop, stock + 4, sizeof(native->outputCrop));
        if (native->outputCrop.num_of_streams > MAX_NUM_STREAMS)
            memset(&native->outputCrop, 0, sizeof(native->outputCrop));
    }
    else if (native->type == CAM_STREAM_PARAM_TYPE_GET_IMG_PROP) {
        memcpy(&native->imgProp, stock + 4, sizeof(native->imgProp));
        int32_t format;
        memcpy(&format, stock + 68, sizeof(format));
        if (mm_camera_oppo_format_from_stock(format, &native->imgProp.format) != 0)
            native->imgProp.format = CAM_FORMAT_MAX;
    }
}
