/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_PREVIEW_BOKEH_FACES_H
#define QCAMERA3_PREVIEW_BOKEH_FACES_H

#include "QCamera3PreviewBokehGeometry.h"

namespace qcamera {

class QCamera3PreviewBokehFaces {
public:
    using Rect = QCamera3PreviewBokehGeometry::Rect;
    static constexpr uint32_t kMaxFaces = 10;
    struct Observation {
        uint32_t cameraHandle = 0, sessionId = 0, nativeFrameNumber = 0, count = 0;
        Rect rectangles[kMaxFaces] = {};
    };

    bool select(const Observation &source, Rect &selected) {
        if (mHaveFrame && int32_t(source.nativeFrameNumber - mLastFrame) <= 0) return false;
        mHaveFrame = true;
        mLastFrame = source.nativeFrameNumber;
        if (!source.count) return false;
        // Stock leaves an old index outside a shortened list; never use that stale slot.
        if (mSelected >= source.count) mSelected = mPrevious = mRepeats = 0;
        const Rect *reference = &source.rectangles[0];
        for (uint32_t i = 0; i < source.count; ++i) {
            const auto &face = source.rectangles[i];
            if (reference->height < face.height || reference->width < face.width)
                reference = &face;
        }
        const Rect *candidateRect = reference;
        uint32_t candidate = 0;
        for (uint32_t i = 0; i < source.count; ++i) {
            const auto &face = source.rectangles[i];
            if (reference->height * 0.75 < face.height || reference->width * 0.75 < face.width) {
                if (candidateRect->height > face.height || candidateRect->width > face.width) {
                    candidateRect = &face;
                    candidate = i;
                }
            }
        }
        if (candidate == mPrevious) {
            if (mRepeats >= 2) mSelected = candidate;
            else ++mRepeats;
        } else mRepeats = 0;
        mPrevious = candidate;
        selected = source.rectangles[mSelected];
        return true;
    }

private:
    uint32_t mSelected = 0, mPrevious = 0, mRepeats = 0, mLastFrame = 0;
    bool mHaveFrame = false;
};

} // namespace qcamera
#endif
