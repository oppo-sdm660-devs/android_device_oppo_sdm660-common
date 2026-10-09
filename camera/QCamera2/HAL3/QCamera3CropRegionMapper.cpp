/* Copyright (c) 2015-2016, 2020, The Linux Foundation. All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are
* met:
*     * Redistributions of source code must retain the above copyright
*       notice, this list of conditions and the following disclaimer.
*     * Redistributions in binary form must reproduce the above
*       copyright notice, this list of conditions and the following
*       disclaimer in the documentation and/or other materials provided
*       with the distribution.
*     * Neither the name of The Linux Foundation nor the names of its
*       contributors may be used to endorse or promote products derived
*       from this software without specific prior written permission.
*
* THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
* WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
* ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
* BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
* CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
* SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
* BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
* WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
* OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
* IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*
*/


#define ATRACE_TAG ATRACE_TAG_CAMERA
#define LOG_TAG "QCamera3CropRegionMapper"

// Camera dependencies
#include "QCamera3CropRegionMapper.h"
#include <limits.h>

extern "C" {
#include "mm_camera_dbg.h"
}

namespace qcamera {

namespace {

// Untrusted extents can overflow int32; clamp both endpoints before scaling.
void boundAxis(int32_t& start, int32_t& extent, int32_t origin, int32_t bound)
{
    const int64_t end = static_cast<int64_t>(start) + (extent > 0 ? extent : 0);
    const int64_t limit = static_cast<int64_t>(origin) + bound;
    const int64_t first = start < origin ? origin : (start > limit ? limit : start);
    const int64_t last = end < first ? first : (end > limit ? limit : end);
    start = static_cast<int32_t>(first);
    extent = static_cast<int32_t>(last - first);
}

int32_t scaleFloor(int32_t value, int32_t output, int32_t input)
{
    return static_cast<int32_t>(static_cast<int64_t>(value) * output / input);
}

int32_t scaleCeil(int32_t value, int32_t output, int32_t input)
{
    return static_cast<int32_t>((static_cast<int64_t>(value) * output + input - 1)
            / input);
}

} // namespace

/*===========================================================================
 * FUNCTION   : QCamera3CropRegionMapper
 *
 * DESCRIPTION: Constructor
 *
 * PARAMETERS : None
 *
 * RETURN     : None
 *==========================================================================*/
QCamera3CropRegionMapper::QCamera3CropRegionMapper()
        : mSensorW(0),
          mSensorH(0),
          mActiveArrayW(0),
          mActiveArrayH(0),
          mActiveArrayLeft(0),
          mActiveArrayTop(0)
{
}

/*===========================================================================
 * FUNCTION   : ~QCamera3CropRegionMapper
 *
 * DESCRIPTION: destructor
 *
 * PARAMETERS : none
 *
 * RETURN     : none
 *==========================================================================*/

QCamera3CropRegionMapper::~QCamera3CropRegionMapper()
{
}

/*===========================================================================
 * FUNCTION   : update
 *
 * DESCRIPTION: update sensor active array size and sensor output size
 *
 * PARAMETERS :
 *   @active_array_w : active array width
 *   @active_array_h : active array height
 *   @sensor_w       : sensor output width
 *   @sensor_h       : sensor output height
 *   @active_array_left : active array origin in pixel array coordinates
 *   @active_array_top  : active array origin in pixel array coordinates
 *
 * RETURN     : none
 *==========================================================================*/
void QCamera3CropRegionMapper::update(uint32_t active_array_w,
        uint32_t active_array_h, uint32_t sensor_w,
        uint32_t sensor_h, int32_t active_array_left, int32_t active_array_top)
{
    // A failed reconfiguration must not reuse the previous sensor mode.
    mSensorW = mSensorH = mActiveArrayW = mActiveArrayH = 0;
    mActiveArrayLeft = mActiveArrayTop = 0;

    // Sanity check
    if (active_array_w == 0 || active_array_h == 0 ||
            sensor_w == 0 || sensor_h == 0) {
        LOGE("active_array size and sensor output size must be non zero");
        return;
    }
    if (active_array_left < 0 || active_array_top < 0 ||
            static_cast<int64_t>(active_array_left) + active_array_w > INT32_MAX ||
            static_cast<int64_t>(active_array_top) + active_array_h > INT32_MAX ||
            active_array_w < sensor_w || active_array_h < sensor_h) {
        LOGE("invalid input: active_array [%d, %d], sensor size [%d, %d]",
                 active_array_w, active_array_h, sensor_w, sensor_h);
        return;
    }
    mSensorW = sensor_w;
    mSensorH = sensor_h;
    mActiveArrayW = active_array_w;
    mActiveArrayH = active_array_h;
    mActiveArrayLeft = active_array_left;
    mActiveArrayTop = active_array_top;

    LOGH("active_array: %d x %d, sensor size %d x %d",
            mActiveArrayW, mActiveArrayH, mSensorW, mSensorH);
}

/*===========================================================================
 * FUNCTION   : toActiveArray
 *
 * DESCRIPTION: Map crop rectangle from sensor output space to active array space
 *
 * PARAMETERS :
 *   @crop_left   : x coordinate of top left corner of rectangle
 *   @crop_top    : y coordinate of top left corner of rectangle
 *   @crop_width  : width of rectangle
 *   @crop_height : height of rectangle
 *
 * RETURN     : none
 *==========================================================================*/
void QCamera3CropRegionMapper::toActiveArray(int32_t& crop_left, int32_t& crop_top,
        int32_t& crop_width, int32_t& crop_height)
{
    if (mSensorW == 0 || mSensorH == 0 ||
            mActiveArrayW == 0 || mActiveArrayH == 0) {
        LOGE("sensor/active array sizes are not initialized!");
        return;
    }

    boundToSize(crop_left, crop_top, crop_width, crop_height,
            mSensorW, mSensorH);
    crop_left = mActiveArrayLeft + scaleFloor(crop_left, mActiveArrayW, mSensorW);
    crop_top = mActiveArrayTop + scaleFloor(crop_top, mActiveArrayH, mSensorH);
    crop_width = scaleFloor(crop_width, mActiveArrayW, mSensorW);
    crop_height = scaleFloor(crop_height, mActiveArrayH, mSensorH);
}

/*===========================================================================
 * FUNCTION   : toSensor
 *
 * DESCRIPTION: Map crop rectangle from active array space to sensor output space
 *
 * PARAMETERS :
 *   @crop_left   : x coordinate of top left corner of rectangle
 *   @crop_top    : y coordinate of top left corner of rectangle
 *   @crop_width  : width of rectangle
 *   @crop_height : height of rectangle
 *
 * RETURN     : none
 *==========================================================================*/

void QCamera3CropRegionMapper::toSensor(int32_t& crop_left, int32_t& crop_top,
        int32_t& crop_width, int32_t& crop_height)
{
    if (mSensorW == 0 || mSensorH == 0 ||
            mActiveArrayW == 0 || mActiveArrayH == 0) {
        LOGE("sensor/active array sizes are not initialized!");
        return;
    }

    boundAxis(crop_left, crop_width, mActiveArrayLeft, mActiveArrayW);
    boundAxis(crop_top, crop_height, mActiveArrayTop, mActiveArrayH);
    const int32_t relative_left = crop_left - mActiveArrayLeft;
    const int32_t relative_top = crop_top - mActiveArrayTop;
    // Round endpoints outwards so an edge-touching crop stays nonempty.
    crop_left = scaleFloor(relative_left, mSensorW, mActiveArrayW);
    crop_top = scaleFloor(relative_top, mSensorH, mActiveArrayH);
    crop_width = crop_width > 0 ?
            scaleCeil(relative_left + crop_width, mSensorW, mActiveArrayW) - crop_left : 0;
    crop_height = crop_height > 0 ?
            scaleCeil(relative_top + crop_height, mSensorH, mActiveArrayH) - crop_top : 0;

    LOGD("before bounding left %d, top %d, width %d, height %d",
         crop_left, crop_top, crop_width, crop_height);
    boundToSize(crop_left, crop_top, crop_width, crop_height,
            mSensorW, mSensorH);
    LOGD("after bounding left %d, top %d, width %d, height %d",
         crop_left, crop_top, crop_width, crop_height);
}

/*===========================================================================
 * FUNCTION   : boundToSize
 *
 * DESCRIPTION: Bound a particular rectangle inside a bounding box
 *
 * PARAMETERS :
 *   @left    : x coordinate of top left corner of rectangle
 *   @top     : y coordinate of top left corner of rectangle
 *   @width   : width of rectangle
 *   @height  : height of rectangle
 *   @bound_w : width of bounding box
 *   @bound_y : height of bounding box
 *
 * RETURN     : none
 *==========================================================================*/
void QCamera3CropRegionMapper::boundToSize(int32_t& left, int32_t& top,
            int32_t& width, int32_t& height, int32_t bound_w, int32_t bound_h)
{
    boundAxis(left, width, 0, bound_w);
    boundAxis(top, height, 0, bound_h);
}

/*===========================================================================
 * FUNCTION   : toActiveArray
 *
 * DESCRIPTION: Map co-ordinate from sensor output space to active array space
 *
 * PARAMETERS :
 *   @x   : x coordinate
 *   @y   : y coordinate
 *
 * RETURN     : none
 *==========================================================================*/
void QCamera3CropRegionMapper::toActiveArray(uint32_t& x, uint32_t& y)
{
    if (mSensorW == 0 || mSensorH == 0 ||
            mActiveArrayW == 0 || mActiveArrayH == 0) {
        LOGE("sensor/active array sizes are not initialized!");
        return;
    }
    if ((x > static_cast<uint32_t>(mSensorW)) ||
            (y > static_cast<uint32_t>(mSensorH))) {
        LOGE("invalid co-ordinate (%d, %d) in (0, 0, %d, %d) space",
                 x, y, mSensorW, mSensorH);
        return;
    }
    x = mActiveArrayLeft + scaleFloor(x, mActiveArrayW, mSensorW);
    y = mActiveArrayTop + scaleFloor(y, mActiveArrayH, mSensorH);
}

/*===========================================================================
 * FUNCTION   : toSensor
 *
 * DESCRIPTION: Map co-ordinate from active array space to sensor output space
 *
 * PARAMETERS :
 *   @x   : x coordinate
 *   @y   : y coordinate
 *
 * RETURN     : none
 *==========================================================================*/

void QCamera3CropRegionMapper::toSensor(uint32_t& x, uint32_t& y)
{
    if (mSensorW == 0 || mSensorH == 0 ||
            mActiveArrayW == 0 || mActiveArrayH == 0) {
        LOGE("sensor/active array sizes are not initialized!");
        return;
    }

    if ((x < static_cast<uint32_t>(mActiveArrayLeft)) ||
            (y < static_cast<uint32_t>(mActiveArrayTop)) ||
            (x > static_cast<uint32_t>(mActiveArrayLeft + mActiveArrayW)) ||
            (y > static_cast<uint32_t>(mActiveArrayTop + mActiveArrayH))) {
        LOGE("invalid co-ordinate (%d, %d) in (%d, %d, %d, %d) space",
                 x, y, mActiveArrayLeft, mActiveArrayTop, mActiveArrayW, mActiveArrayH);
        return;
    }
    x = scaleFloor(x - mActiveArrayLeft, mSensorW, mActiveArrayW);
    y = scaleFloor(y - mActiveArrayTop, mSensorH, mActiveArrayH);
}

void QCamera3CropRegionMapper::convertFDROI(int32_t& crop_left, int32_t& crop_top,
        int32_t& crop_width, int32_t& crop_height)
{
    if (mSensorW == 0 || mSensorH == 0 ||
            mActiveArrayW == 0 || mActiveArrayH == 0) {
        LOGE("sensor/active array sizes are not initialized!");
        return;
    }

    boundToSize(crop_left, crop_top, crop_width, crop_height,
            mSensorW, mSensorH);
    int32_t new_width, new_height;
    if (static_cast<int64_t>(mActiveArrayW) * mSensorH <=
            static_cast<int64_t>(mActiveArrayH) * mSensorW) {
        new_width = mActiveArrayW;
        new_height = scaleFloor(mSensorH, mActiveArrayW, mSensorW);
    } else {
        new_height = mActiveArrayH;
        new_width = scaleFloor(mSensorW, mActiveArrayH, mSensorH);
    }
    const int32_t new_left = mActiveArrayLeft + (mActiveArrayW - new_width) / 2;
    const int32_t new_top = mActiveArrayTop + (mActiveArrayH - new_height) / 2;
    crop_left = new_left + scaleFloor(crop_left, new_width, mSensorW);
    crop_top = new_top + scaleFloor(crop_top, new_height, mSensorH);
    crop_width = scaleFloor(crop_width, new_width, mSensorW);
    crop_height = scaleFloor(crop_height, new_height, mSensorH);
}

}; // namespace qcamera
