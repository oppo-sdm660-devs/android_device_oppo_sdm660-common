/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_SOURCE_METADATA_H
#define QCAMERA3_SOURCE_METADATA_H
#include "cam_intf.h"
#include <memory>
#include <cstring>

namespace qcamera {
inline std::shared_ptr<metadata_buffer_t> pairSourceMetadata(
        const std::shared_ptr<metadata_buffer_t> &full,
        const std::shared_ptr<metadata_buffer_t> &urgent, int64_t timestampNs)
{
    if (!full || !urgent || !full->is_valid[CAM_INTF_META_SENSOR_TIMESTAMP] ||
            *POINTER_OF_META(CAM_INTF_META_SENSOR_TIMESTAMP, full.get()) != timestampNs ||
            !full->is_valid[CAM_INTF_META_SENSOR_EXPOSURE_TIME] ||
            !full->is_valid[CAM_INTF_META_SENSOR_SENSITIVITY] ||
            !urgent->is_valid[CAM_INTF_META_AEC_STATE] ||
            !urgent->is_valid[CAM_INTF_META_AWB_STATE] ||
            !urgent->is_valid[CAM_INTF_META_AF_STATE]) return nullptr;
    auto merged = std::make_shared<metadata_buffer_t>(*full);
    // Urgent 3A belongs to its own frame, which can differ from the normal frame.
#define MERGE_SOURCE_URGENT(tag) do { \
    if (urgent->is_valid[tag]) { \
        memcpy(POINTER_OF_META(tag, merged.get()), \
                POINTER_OF_META(tag, urgent.get()), SIZE_OF_PARAM(tag, merged.get())); \
        merged->is_valid[tag] = 1; \
    } \
} while (0)
    MERGE_SOURCE_URGENT(CAM_INTF_META_AWB_STATE);
    MERGE_SOURCE_URGENT(CAM_INTF_META_AEC_PRECAPTURE_TRIGGER);
    MERGE_SOURCE_URGENT(CAM_INTF_META_AEC_STATE);
    MERGE_SOURCE_URGENT(CAM_INTF_META_AEC_INFO);
    MERGE_SOURCE_URGENT(CAM_INTF_META_AF_STATE);
    MERGE_SOURCE_URGENT(CAM_INTF_PARM_FOCUS_MODE);
    MERGE_SOURCE_URGENT(CAM_INTF_META_AF_TRIGGER);
    MERGE_SOURCE_URGENT(CAM_INTF_PARM_WHITE_BALANCE);
    MERGE_SOURCE_URGENT(CAM_INTF_META_AWB_INFO);
    MERGE_SOURCE_URGENT(CAM_INTF_META_AEC_MODE);
    MERGE_SOURCE_URGENT(CAM_INTF_PARM_LED_MODE);
    MERGE_SOURCE_URGENT(CAM_INTF_PARM_REDEYE_REDUCTION);
#undef MERGE_SOURCE_URGENT
    return merged;
}
} // namespace qcamera
#endif
