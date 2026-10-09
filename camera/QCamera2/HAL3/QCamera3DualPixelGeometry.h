/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_DUAL_PIXEL_GEOMETRY_H
#define QCAMERA3_DUAL_PIXEL_GEOMETRY_H

#include "QCamera3HdrAlgo.h"
#include <cmath>
#include <errno.h>
#include <initializer_list>

namespace qcamera {

// SNAP_CROP_INFO is a sequence of crop+scale operations, not four rectangles
// in SCALER_CROP_REGION coordinates. Keep fractional edges until sampling.
struct QCamera3DualPixelGeometry {
    struct Rect { double left, top, width, height; };
    struct Stage { Rect input, crop; };
    Stage sensor, camif, isp, cpp;
    double ispScaleRatio = 0;
    uint32_t outputWidth = 0, outputHeight = 0;
    uint32_t clockwiseRotation = 0;

    static bool valid(const Rect &r) {
        return std::isfinite(r.left) && std::isfinite(r.top) &&
                std::isfinite(r.width) && std::isfinite(r.height) &&
                r.left >= 0 && r.top >= 0 && r.width > 0 && r.height > 0 &&
                r.left + r.width <= 32768 && r.top + r.height <= 32768;
    }
    static bool contains(const Rect &outer, const Rect &inner) {
        return valid(outer) && valid(inner) && inner.left >= outer.left &&
                inner.top >= outer.top && inner.left + inner.width <= outer.left + outer.width &&
                inner.top + inner.height <= outer.top + outer.height;
    }
    bool valid() const {
        if (!std::isfinite(ispScaleRatio) || ispScaleRatio <= 0 || ispScaleRatio > 64 ||
                !outputWidth || !outputHeight || (outputWidth | outputHeight) & 1 ||
                outputWidth > 16384 || outputHeight > 16384 ||
                (clockwiseRotation != 0 && clockwiseRotation != 90 &&
                 clockwiseRotation != 180 && clockwiseRotation != 270)) return false;
        for (const Stage *s : {&sensor, &camif, &isp, &cpp})
            if (!contains(s->input, s->crop) || s->input.left != 0 || s->input.top != 0)
                return false;
        return true;
    }
    Rect sensorWindow() const {
        if (!valid()) return {};
        // Work backwards: CPP input is ISP output; ISP input is the scaler
        // output; scaler covers CAMIF; CAMIF input is the binned sensor output.
        Rect r = cpp.crop;
        const auto back = [](Rect r, const Rect &output, const Rect &crop) {
            return Rect{crop.left + r.left * crop.width / output.width,
                crop.top + r.top * crop.height / output.height,
                r.width * crop.width / output.width, r.height * crop.height / output.height};
        };
        r = back(r, cpp.input, isp.crop);
        // ISP48 uses the reported scaling_ratio for both axes. Deriving two
        // ratios from rounded scaler dimensions loses its fractional ROI.
        r = {camif.crop.left + r.left * ispScaleRatio,
            camif.crop.top + r.top * ispScaleRatio,
            r.width * ispScaleRatio, r.height * ispScaleRatio};
        return back(r, camif.input, sensor.crop);
    }
    // Coordinates are continuous pixel edges; callers add 0.5 for pixel centres.
    bool toSensor(double x, double y, double &sx, double &sy) const {
        if (!valid() || x < 0 || y < 0 || x > outputWidth || y > outputHeight) return false;
        double u = x / outputWidth, v = y / outputHeight;
        if (clockwiseRotation == 90) { const double t = u; u = v; v = 1 - t; }
        else if (clockwiseRotation == 180) { u = 1 - u; v = 1 - v; }
        else if (clockwiseRotation == 270) { const double t = u; u = 1 - v; v = t; }
        const Rect r = sensorWindow();
        sx = r.left + u * r.width; sy = r.top + v * r.height;
        return true;
    }
    bool fromSensor(double sx, double sy, double &x, double &y) const {
        const Rect r = sensorWindow();
        if (!valid(r) || sx < r.left || sy < r.top ||
                sx > r.left + r.width || sy > r.top + r.height) return false;
        double u = (sx - r.left) / r.width, v = (sy - r.top) / r.height;
        if (clockwiseRotation == 90) { const double t = u; u = 1 - v; v = t; }
        else if (clockwiseRotation == 180) { u = 1 - u; v = 1 - v; }
        else if (clockwiseRotation == 270) { const double t = u; u = v; v = 1 - t; }
        x = u * outputWidth; y = v * outputHeight;
        return true;
    }
    // ARC's four reference dimensions cannot express a translated principal
    // point. Only resample a reference window wholly covered by real pixels.
    int32_t referenceImage(const QCamera3HdrAlgo::Image &source,
            const Rect &reference, const QCamera3HdrAlgo::Image &out) const {
        if (!valid() || !QCamera3HdrAlgo::validImage(source) ||
                !QCamera3HdrAlgo::validImage(out) || source.width != outputWidth ||
                source.height != outputHeight || source.data == out.data) return -EINVAL;
        if (!contains(sensorWindow(), reference)) return -ENOTSUP;
        const Rect window = sensorWindow();
        for (unsigned plane = 0; plane < 2; ++plane) {
            const unsigned unit = plane ? 2 : 1;
            const unsigned rows = out.height / unit, columns = out.width / unit;
            const unsigned sourceColumns = source.width / unit, sourceRows = source.height / unit;
            for (unsigned y = 0; y < rows; ++y)
                for (unsigned x = 0; x < columns; ++x) {
                    const double sx = reference.left + (x + 0.5) * reference.width / columns;
                    const double sy = reference.top + (y + 0.5) * reference.height / rows;
                    double u = (sx - window.left) / window.width;
                    double v = (sy - window.top) / window.height;
                    if (clockwiseRotation == 90) { const double t = u; u = 1 - v; v = t; }
                    else if (clockwiseRotation == 180) { u = 1 - u; v = 1 - v; }
                    else if (clockwiseRotation == 270) { const double t = u; u = v; v = 1 - t; }
                    double px = u * sourceColumns - 0.5, py = v * sourceRows - 0.5;
                    px = px < 0 ? 0 : px > sourceColumns - 1 ? sourceColumns - 1 : px;
                    py = py < 0 ? 0 : py > sourceRows - 1 ? sourceRows - 1 : py;
                    const unsigned ix = unsigned(px), iy = unsigned(py);
                    const unsigned jx = ix + 1 < sourceColumns ? ix + 1 : ix;
                    const unsigned jy = iy + 1 < sourceRows ? iy + 1 : iy;
                    const double fx = px - ix, fy = py - iy;
                    const size_t offset = plane ? source.uvOffset : source.yOffset;
                    const unsigned stride = plane ? source.uvStride : source.yStride;
                    const size_t to = (plane ? out.uvOffset : out.yOffset) +
                            size_t(y) * (plane ? out.uvStride : out.yStride) + x * unit;
                    for (unsigned c = 0; c < unit; ++c) {
                        const double top = source.data[offset + size_t(iy) * stride + ix * unit + c] * (1 - fx) +
                                source.data[offset + size_t(iy) * stride + jx * unit + c] * fx;
                        const double bottom = source.data[offset + size_t(jy) * stride + ix * unit + c] * (1 - fx) +
                                source.data[offset + size_t(jy) * stride + jx * unit + c] * fx;
                        out.data[to + c] = uint8_t(std::floor(top * (1 - fy) + bottom * fy + 0.5));
                    }
                }
        }
        return 0;
    }
};
} // namespace qcamera
#endif
