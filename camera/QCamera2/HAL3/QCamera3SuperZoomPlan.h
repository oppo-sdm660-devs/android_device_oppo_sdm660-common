/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_SUPER_ZOOM_PLAN_H
#define QCAMERA3_SUPER_ZOOM_PLAN_H
#include "QCamera3StockSuperPhoto.h"
#include <limits.h>

namespace qcamera {
struct QCamera3SuperZoomPlan {
    struct Rect { int32_t left, top, width, height; };
    struct Conditions {
        bool engineAvailable = false;
        bool rearSingleSensor = false;
        bool automaticExposure = false;
        bool flashOff = false;
        bool video = false;
        bool reprocess = false;
        bool raw = false;
        bool otherNativeStage = false;
    };
    enum { FRAME_COUNT = QCamera3StockSuperPhoto::ZOOM_FRAME_COUNT };
    Rect captureCrop = {};
    Rect resultCrop = {};
    uint32_t outputWidth = 0, outputHeight = 0;
    float zoomRatio = 2.0f;

    static bool geometry(const Rect &active, uint32_t width, uint32_t height,
            Rect &source, Rect &result) {
        if (!valid(active) || !width || !height || width > INT_MAX || height > INT_MAX)
            return false;
        uint32_t a = width, b = height;
        while (b) { const uint32_t next = a % b; a = b; b = next; }
        const uint32_t unitWidth = width / a, unitHeight = height / a;
        const uint32_t scale = uint32_t(active.width) / unitWidth < uint32_t(active.height) / unitHeight ?
                uint32_t(active.width) / unitWidth : uint32_t(active.height) / unitHeight;
        if (!scale) return false;
        source.width = int32_t(unitWidth * scale);
        source.height = int32_t(unitHeight * scale);
        source.left = active.left + (active.width - source.width) / 2;
        source.top = active.top + (active.height - source.height) / 2;
        result.width = source.width / 2 + source.width % 2;
        result.height = source.height / 2 + source.height % 2;
        result.left = source.left + (source.width - result.width) / 2;
        result.top = source.top + (source.height - result.height) / 2;
        return true;
    }

    // Standard crop/zoom alone never opts another client into a multi-frame path.
    static bool make(bool explicit2x, const Conditions &conditions, const Rect &active,
            const Rect &requestedCrop, uint32_t width, uint32_t height,
            QCamera3SuperZoomPlan &plan) {
        if (!explicit2x || !conditions.engineAvailable || !conditions.rearSingleSensor ||
                !conditions.automaticExposure || !conditions.flashOff || conditions.video ||
                conditions.reprocess || conditions.raw || conditions.otherNativeStage ||
                !valid(active) || !valid(requestedCrop) || !width || !height ||
                (width & 1) || (height & 1) || width > INT_MAX || height > INT_MAX)
            return false;
        Rect source = {}, result = {};
        if (!geometry(active, width, height, source, result) ||
                requestedCrop.left != result.left || requestedCrop.top != result.top ||
                requestedCrop.width != result.width || requestedCrop.height != result.height)
            return false;
        QCamera3SuperZoomPlan output;
        output.captureCrop = source;
        output.resultCrop = requestedCrop;
        output.outputWidth = width;
        output.outputHeight = height;
        plan = output;
        return true;
    }
    bool sourceHasFullFieldOfView(const Rect &actualCrop) const {
        return actualCrop.left == captureCrop.left && actualCrop.top == captureCrop.top &&
                actualCrop.width == captureCrop.width && actualCrop.height == captureCrop.height;
    }
private:
    static bool valid(const Rect &rect) {
        return rect.left >= 0 && rect.top >= 0 && rect.width > 0 && rect.height > 0 &&
                int64_t(rect.left) + rect.width <= INT_MAX &&
                int64_t(rect.top) + rect.height <= INT_MAX;
    }
};
}
#endif
