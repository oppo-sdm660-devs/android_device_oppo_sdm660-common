/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "mm_camera_oppo_metadata.h"
#include "mm_camera_oppo_format.h"
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* Descriptor mappings are CPU-only; image buffers use ION. */
int mm_camera_oppo_buffer_alloc(mm_camera_oppo_buffer_t *buffer, size_t size)
{
    /* Bionic selects the syscall for the process ABI, not the kernel ARCH. */
    int fd = memfd_create("oppo-camera-wire", MFD_CLOEXEC);
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
        /* Stock EIS2 is implemented by IS + C2D. EIS3/vendor topologies and
         * their private PP payloads are not part of this bridge. */
        if (info->is_type[i] != IS_TYPE_NONE &&
                (info->is_type[i] != IS_TYPE_EIS_2_0 ||
                 (info->type[i] != CAM_STREAM_TYPE_PREVIEW &&
                  info->type[i] != CAM_STREAM_TYPE_VIDEO)))
            return -ENOTSUP;
        const cam_frame_margins_t *margin = &info->margins[i];
        if (!isfinite(margin->widthMargins) ||
                !isfinite(margin->heightMargins) ||
                margin->widthMargins < 0 || margin->heightMargins < 0)
            return -EINVAL;
        if (info->is_type[i] == IS_TYPE_EIS_2_0 &&
                (info->stream_sizes[i].width <= 0 ||
                 info->stream_sizes[i].height <= 0 ||
                 (1.0 + margin->widthMargins) * info->stream_sizes[i].width > INT32_MAX ||
                 (1.0 + margin->heightMargins) * info->stream_sizes[i].height > INT32_MAX))
            return -EINVAL;
        if (encode) {
            int32_t format;
            if (mm_camera_oppo_format_to_stock(info->format[i], &format) != 0)
                return -EINVAL;
            info->format[i] = (cam_format_t)format;
            info->postprocess_mask[i] &= CAM_QCOM_FEATURE_CROP |
                    CAM_QCOM_FEATURE_ROTATION | CAM_QCOM_FEATURE_FLIP |
                    CAM_QCOM_FEATURE_SCALE | CAM_QCOM_FEATURE_DENOISE2D |
                    CAM_QCOM_FEATURE_SHARPNESS | CAM_QCOM_FEATURE_CAC |
                    CAM_QCOM_FEATURE_EFFECT | CAM_QCOM_FEATURE_CPP_TNR |
                    CAM_QCOM_FEATURE_SENSOR_HDR | CAM_QCOM_FEATURE_ZIGZAG_HDR |
                    CAM_QCOM_FEATURE_STAGGERED_VIDEO_HDR;
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
            if (entry->native_id == CAM_INTF_META_STREAM_INFO ||
                    entry->native_id == CAM_INTF_META_STREAM_INFO_FOR_PIC_RES) {
                int result = mm_camera_oppo_stream_sizes((cam_stream_size_info_t *)
                        (stock + entry->stock_offset), 1);
                if (result != 0)
                    return result;
            }
            stock[entry->stock_id] = 1;
        }
    }
    return 0;
}

/* Stock FD records have an eight-byte private tail per face. A flat copy of
 * the 344-byte stock payload would corrupt CLO's 264-byte result. */
_Static_assert(sizeof(cam_face_detection_info_t) == 24, "Native face record");
_Static_assert(offsetof(cam_face_detection_info_t, face_boundary) == 8,
        "Native face rectangle");
/* Stock MCT get_pointer_of/get_size_of: IDs 215/218 are 0x12e0/30
 * and 0x13dc/360. Stock HAL3 convertLandmarks and imglib's face producer
 * agree on the three validity bytes and x/y uint32 pairs at 0/4,12/16,24/28. */
_Static_assert(sizeof(cam_face_landmarks_info_t) == 36, "Face landmark ABI");
_Static_assert(offsetof(cam_face_landmarks_info_t, left_eye_center) == 4 &&
        offsetof(cam_face_landmarks_info_t, right_eye_center) == 16 &&
        offsetof(cam_face_landmarks_info_t, mouth_center) == 28, "Face landmark fields");
_Static_assert(sizeof(cam_face_blink_info_t) == 3, "Face blink ABI");

static int mm_camera_oppo_face_point_valid(uint8_t flag,
        cam_coordinate_type_t point, const cam_rect_t *rect)
{
    return flag == 1 && point.x <= INT32_MAX && point.y <= INT32_MAX &&
            (int64_t)point.x >= rect->left && (int64_t)point.y >= rect->top &&
            (int64_t)point.x < (int64_t)rect->left + rect->width &&
            (int64_t)point.y < (int64_t)rect->top + rect->height;
}

static void mm_camera_oppo_faces_decode(const unsigned char *stock,
        metadata_buffer_t *native)
{
    enum { FACE_ID = 17, FACE_OFFSET = 0x1138, FACE_COUNT = 10, FACE_STRIDE = 32 };
    if (!stock[FACE_ID])
        return;
    const unsigned char *data = stock + FACE_OFFSET;
    cam_face_detection_data_t *faces =
            &native->data.member_variable_CAM_INTF_META_FACE_DETECTION[0];
    uint32_t type;
    memcpy(&type, data + 328, sizeof(type));
    if (type != QCAMERA_FD_PREVIEW && type != QCAMERA_FD_SNAPSHOT)
        return;
    memcpy(&faces->frame_id, data, sizeof(faces->frame_id));
    faces->fd_type = (qcamera_face_detect_type_t)type;
    memcpy(&faces->fd_frame_dim, data + 332, sizeof(faces->fd_frame_dim));
    if (faces->fd_frame_dim.width < 0 || faces->fd_frame_dim.height < 0)
        return;
    faces->update_flag = data[340] != 0;
    unsigned count = data[4];
    if (count > FACE_COUNT)
        count = FACE_COUNT;
    if (count > MAX_ROI)
        count = MAX_ROI;
    for (unsigned i = 0; i < count; ++i) {
        const unsigned char *record = data + 8 + i * FACE_STRIDE;
        cam_face_detection_info_t face = {0};
        memcpy(&face.face_id, record, sizeof(face.face_id));
        memcpy(&face.score, record + 4, sizeof(face.score));
        memcpy(&face.face_boundary, record + 8, sizeof(face.face_boundary));
        const cam_rect_t *rect = &face.face_boundary;
        int64_t right = (int64_t)rect->left + rect->width;
        int64_t bottom = (int64_t)rect->top + rect->height;
        if (face.score <= 0 || rect->width <= 0 || rect->height <= 0 ||
                right <= 0 || bottom <= 0 || right > INT32_MAX || bottom > INT32_MAX)
            continue;
        if (face.score > 100)
            face.score = 100;
        /* Keep the backend coordinate space; the HAL3 crop mapper clips and
         * converts sensor-output rectangles to active-array coordinates. */
        unsigned slot = faces->num_faces_detected++;
        faces->faces[slot] = face;
        /* Auxiliary arrays use the original backend index. Compact them with
         * the surviving rectangle, never with a skipped/malformed face. */
        if (stock[218]) {
            cam_face_landmarks_info_t points;
            memcpy(&points, stock + 0x13dc + i * 36, sizeof(points));
            points.is_left_eye_valid = mm_camera_oppo_face_point_valid(
                    points.is_left_eye_valid, points.left_eye_center, rect);
            points.is_right_eye_valid = mm_camera_oppo_face_point_valid(
                    points.is_right_eye_valid, points.right_eye_center, rect);
            points.is_mouth_valid = mm_camera_oppo_face_point_valid(
                    points.is_mouth_valid, points.mouth_center, rect);
            if (!points.is_left_eye_valid) memset(&points.left_eye_center, 0, sizeof(points.left_eye_center));
            if (!points.is_right_eye_valid) memset(&points.right_eye_center, 0, sizeof(points.right_eye_center));
            if (!points.is_mouth_valid) memset(&points.mouth_center, 0, sizeof(points.mouth_center));
            native->data.member_variable_CAM_INTF_META_FACE_LANDMARK[0].face_landmarks[slot] = points;
        }
        if (stock[215]) {
            cam_face_blink_info_t blink;
            memcpy(&blink, stock + 0x12e0 + i * 3, sizeof(blink));
            /* 255 is an explicit unknown sentinel for malformed degrees. */
            if (blink.blink_detected > 1 || blink.left_blink > 100 || blink.right_blink > 100)
                blink = (cam_face_blink_info_t) { 0, 255, 255 };
            native->data.member_variable_CAM_INTF_META_FACE_BLINK[0].blink[slot] = blink;
        }
    }
    /* A valid zero-face result clears stale rectangles in Camera2. */
    native->is_valid[CAM_INTF_META_FACE_DETECTION] = 1;
    native->is_valid[CAM_INTF_META_FACE_LANDMARK] = stock[218] != 0;
    native->is_valid[CAM_INTF_META_FACE_BLINK] = stock[215] != 0;
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
    mm_camera_oppo_faces_decode(stock, native);
    /* R11/R11s tag 66 is a distinct 116-byte AEC record at 0x1082c.
     * HAL1 consumes its last scalar (+0x70) as LLHDR DRC gain. Extract only
     * that verified field; the native cam_3a_params_t is a different ABI. */
    _Static_assert(0x1082c + 116 <= MM_CAMERA_OPPO_METADATA_SIZE,
            "Stock LLHDR DRC record bounds");
    if (stock[66]) {
        float gain;
        memcpy(&gain, stock + 0x1089c, sizeof(gain));
        if (isfinite(gain) && gain > 0.0f) {
            native->oppo_algorithm.llhdr_drc_gain = gain;
            native->oppo_algorithm.llhdr_drc_valid = 1;
        }
    }
}
