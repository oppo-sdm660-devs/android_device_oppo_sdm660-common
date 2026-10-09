/* SPDX-License-Identifier: BSD-3-Clause */
#include "mm_camera_oppo_metadata.h"
#include "mm_camera_oppo_format.h"
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

/* Descriptor mappings are CPU-only; image buffers use ION. */
int mm_camera_oppo_buffer_alloc(mm_camera_oppo_buffer_t *buffer, size_t size)
{
    int fd = syscall(__NR_memfd_create, "oppo-camera-wire", 1 /* MFD_CLOEXEC */);
    if (fd < 0)
        return -errno;
    if (ftruncate(fd, size) < 0) {
        int result = -errno;
        close(fd);
        return result;
    }
    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        int result = -errno;
        close(fd);
        return result;
    }
    buffer->fd = fd;
    buffer->size = size;
    buffer->data = data;
    return 0;
}

void mm_camera_oppo_buffer_release(mm_camera_oppo_buffer_t *buffer)
{
    /* Unallocated owners must not close fd 0. */
    if (buffer->data != NULL) {
        munmap(buffer->data, buffer->size);
        close(buffer->fd);
    }
    memset(buffer, 0, sizeof(*buffer));
    buffer->fd = -1;
}

typedef struct {
    unsigned native_id;
    unsigned stock_id;
    unsigned native_offset;
    unsigned stock_offset;
    unsigned size;
} oppo_metadata_entry_t;

#define OPPO_META(name, id, offset, bytes) \
    { name, id, offsetof(metadata_buffer_t, data.member_variable_##name), offset, bytes },
static const oppo_metadata_entry_t entries[] = {
#include "mm_camera_oppo_metadata_map.inc"
};
#undef OPPO_META

/* Stock RAW_DIMENSION contains only width and height. */
#define OPPO_META(name, id, offset, bytes) \
    _Static_assert(sizeof(((metadata_buffer_t *)0)->data.member_variable_##name) == bytes \
        || (name == CAM_INTF_PARM_RAW_DIMENSION \
            && sizeof(((metadata_buffer_t *)0)->data.member_variable_##name) == 12), \
        "CLO metadata payload changed: " #name); \
    _Static_assert(offset + bytes <= MM_CAMERA_OPPO_METADATA_SIZE, "Stock metadata bounds");
#include "mm_camera_oppo_metadata_map.inc"
#undef OPPO_META

/* The stock stream-size layout matches the native structure; formats differ. */
_Static_assert(sizeof(cam_stream_size_info_t) == 528, "Stream size ABI changed");
_Static_assert(offsetof(cam_stream_size_info_t, num_streams) == 64, "Stream count ABI");
_Static_assert(offsetof(cam_stream_size_info_t, type) == 68, "Stream type ABI");
_Static_assert(offsetof(cam_stream_size_info_t, postprocess_mask) == 104, "Stream PP ABI");
_Static_assert(offsetof(cam_stream_size_info_t, format) == 212, "Stream format ABI");
_Static_assert(offsetof(cam_stream_size_info_t, rotation) == 244, "Stream rotation ABI");

static int mm_camera_oppo_stream_sizes(cam_stream_size_info_t *info, int encode)
{
    if (info->num_streams > MAX_NUM_STREAMS)
        return -EINVAL;
    for (unsigned i = 0; i < info->num_streams; ++i) {
        if (encode) {
            int32_t format;
            if (mm_camera_oppo_format_to_stock(info->format[i], &format) != 0)
                return -EINVAL;
            info->format[i] = (cam_format_t)format;
            info->postprocess_mask[i] &= CAM_QCOM_FEATURE_CROP |
                    CAM_QCOM_FEATURE_ROTATION | CAM_QCOM_FEATURE_FLIP |
                    CAM_QCOM_FEATURE_SCALE;
            info->is_type[i] = IS_TYPE_NONE;
            if (info->type[i] == CAM_STREAM_TYPE_METADATA) {
                info->stream_sizes[i].width = MM_CAMERA_OPPO_METADATA_SIZE;
                info->stream_sizes[i].height = 1;
            }
        } else {
            cam_format_t format;
            if (mm_camera_oppo_format_from_stock(info->format[i], &format) != 0)
                return -EINVAL;
            info->format[i] = format;
            if (info->type[i] == CAM_STREAM_TYPE_METADATA) {
                info->stream_sizes[i].width = sizeof(metadata_buffer_t);
                info->stream_sizes[i].height = 1;
            }
        }
    }
    if (encode) {
        info->sync_type = CAM_TYPE_STANDALONE;
        memset(info->margins, 0, sizeof(info->margins));
        info->is_secure = 0;
    }
    return 0;
}

int mm_camera_oppo_metadata_encode(const metadata_buffer_t *native, void *wire)
{
    unsigned char *stock = wire;
    const unsigned char *source = (const unsigned char *)native;
    memset(stock, 0, MM_CAMERA_OPPO_METADATA_SIZE);
    if (native == NULL)
        return 0;
    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); ++i) {
        const oppo_metadata_entry_t *entry = &entries[i];
        if (native->is_valid[entry->native_id]) {
            memcpy(stock + entry->stock_offset, source + entry->native_offset, entry->size);
            if ((entry->native_id == CAM_INTF_META_STREAM_INFO ||
                    entry->native_id == CAM_INTF_META_STREAM_INFO_FOR_PIC_RES) &&
                    mm_camera_oppo_stream_sizes((cam_stream_size_info_t *)
                        (stock + entry->stock_offset), 1) != 0)
                return -EINVAL;
            stock[entry->stock_id] = 1;
        }
    }
    return 0;
}

void mm_camera_oppo_metadata_decode(const void *wire, metadata_buffer_t *native)
{
    const unsigned char *stock = wire;
    unsigned char *target = (unsigned char *)native;
    memset(native, 0, sizeof(*native));
    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); ++i) {
        const oppo_metadata_entry_t *entry = &entries[i];
        if (stock[entry->stock_id]) {
            memcpy(target + entry->native_offset, stock + entry->stock_offset, entry->size);
            if ((entry->native_id == CAM_INTF_META_STREAM_INFO ||
                    entry->native_id == CAM_INTF_META_STREAM_INFO_FOR_PIC_RES) &&
                    mm_camera_oppo_stream_sizes((cam_stream_size_info_t *)
                        (target + entry->native_offset), 0) != 0)
                continue;
            native->is_valid[entry->native_id] = 1;
        }
    }
}
