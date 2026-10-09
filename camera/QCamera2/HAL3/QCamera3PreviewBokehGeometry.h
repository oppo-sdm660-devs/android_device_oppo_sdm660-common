/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_PREVIEW_BOKEH_GEOMETRY_H
#define QCAMERA3_PREVIEW_BOKEH_GEOMETRY_H

#include "QCamera3HdrAlgo.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <errno.h>
#include <vector>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace qcamera {

class QCamera3PreviewBokehGeometry {
public:
    using Image = QCamera3HdrAlgo::Image;
    struct Rect { double left = 0, top = 0, width = 0, height = 0; };
    struct Frame {
        Rect sensorWindow, referenceWindow;
        uint32_t width = 0, height = 0;
    };

    // One plan per camera: rebuild only when the native image geometry changes.
    int32_t configure(const Frame &frame) {
        if (mConfigured && same(mFrame, frame)) return 0;
        if (!valid(frame)) return -ENOTSUP;
        mFrame = frame;
        for (unsigned plane = 0; plane < 2; ++plane) {
            const uint32_t columns = frame.width >> plane, rows = frame.height >> plane;
            axes(mToReference[plane].x, columns, frame.sensorWindow.left,
                    frame.sensorWindow.width, frame.referenceWindow.left,
                    frame.referenceWindow.width, false);
            axes(mToReference[plane].y, rows, frame.sensorWindow.top,
                    frame.sensorWindow.height, frame.referenceWindow.top,
                    frame.referenceWindow.height, false);
            axes(mToSource[plane].x, columns, frame.referenceWindow.left,
                    frame.referenceWindow.width, frame.sensorWindow.left,
                    frame.sensorWindow.width, true);
            axes(mToSource[plane].y, rows, frame.referenceWindow.top,
                    frame.referenceWindow.height, frame.sensorWindow.top,
                    frame.sensorWindow.height, true);
            auto &plan = mToSource[plane];
            plan.begin = 0;
            while (plan.begin < plan.x.size() && !plan.x[plan.begin].active) ++plan.begin;
            plan.end = plan.x.size();
            while (plan.end > plan.begin && !plan.x[plan.end - 1].active) --plan.end;
            blocks(mToReference[plane], frame.width, plane ? 2 : 1);
            blocks(mToSource[plane], frame.width, plane ? 2 : 1);
        }
        mRows[0].resize(frame.width); mRows[1].resize(frame.width);
        mConfigured = true;
        return 0;
    }

    static bool valid(const Frame &frame) {
        return frame.width && frame.height && !(frame.width & 1) && !(frame.height & 1) &&
                frame.width <= 16384 && frame.height <= 16384 &&
                contains(frame.sensorWindow, frame.referenceWindow);
    }
    static bool contains(const Rect &outer, const Rect &inner) {
        return validRect(outer) && validRect(inner) && inner.left >= outer.left &&
                inner.top >= outer.top && inner.left + inner.width <= outer.left + outer.width &&
                inner.top + inner.height <= outer.top + outer.height;
    }
    bool identity() const {
        return sameRect(mFrame.sensorWindow, mFrame.referenceWindow);
    }
    static bool focus(const Frame &frame, int32_t sensorX, int32_t sensorY,
            int32_t &imageX, int32_t &imageY) {
        if (!valid(frame)) return false;
        const auto &r = frame.referenceWindow;
        if (sensorX < r.left || sensorY < r.top ||
                sensorX >= r.left + r.width || sensorY >= r.top + r.height) return false;
        imageX = std::min<int32_t>(frame.width - 1,
                std::lround((sensorX - r.left) * frame.width / r.width));
        imageY = std::min<int32_t>(frame.height - 1,
                std::lround((sensorY - r.top) * frame.height / r.height));
        return true;
    }

    int32_t referenceImage(const Image &source, const Image &destination) const {
        if (!images(source, destination) || source.data == destination.data) return -EINVAL;
        for (unsigned plane = 0; plane < 2; ++plane) {
            const auto &plan = mToReference[plane];
            const unsigned unit = plane ? 2 : 1;
            int64_t cached[2] = {-1, -1};
            for (size_t y = 0; y < plan.y.size(); ++y) {
                auto *row = destination.data + (plane ? destination.uvOffset : destination.yOffset) +
                        y * (plane ? destination.uvStride : destination.yStride);
                const auto &ay = plan.y[y];
                const unsigned top = cachedRow(cached, ay.lower, ay.upper, source, nullptr, plane, plan);
                const unsigned bottom = cachedRow(cached, ay.upper, ay.lower, source, nullptr, plane, plan);
                vertical(mRows[top], mRows[bottom], ay.weight, 0, plan.x.size() * unit,
                        nullptr, row, nullptr);
            }
        }
        return 0;
    }

    // Project only the effect residual. An ARC passthrough leaves every source
    // byte unchanged, including the original field of view outside calibration.
    int32_t project(const Image &source, const Image &reference, const Image &processed,
            const Image &destination, bool &changed) const {
        changed = false;
        if (!images(source, destination) || !images(reference, processed) ||
                source.data == destination.data) return -EINVAL;
        for (unsigned plane = 0; plane < 2; ++plane) {
            const auto &plan = mToSource[plane];
            const unsigned unit = plane ? 2 : 1;
            int64_t cached[2] = {-1, -1};
            for (size_t y = 0; y < plan.y.size(); ++y) {
                const auto *original = source.data + (plane ? source.uvOffset : source.yOffset) +
                        y * (plane ? source.uvStride : source.yStride);
                auto *row = destination.data + (plane ? destination.uvOffset : destination.yOffset) +
                        y * (plane ? destination.uvStride : destination.yStride);
                memcpy(row, original, mFrame.width);
                if (!plan.y[y].active || plan.begin == plan.end) continue;
                const auto &ay = plan.y[y];
                const unsigned top = cachedRow(cached, ay.lower, ay.upper, processed, &reference, plane, plan);
                const unsigned bottom = cachedRow(cached, ay.upper, ay.lower, processed, &reference, plane, plan);
                vertical(mRows[top], mRows[bottom], ay.weight, plan.begin * unit, plan.end * unit,
                        original, row, &changed);
            }
        }
        return 0;
    }

private:
    struct Axis { uint32_t lower, upper, weight; bool active; };
    struct ByteAxis { uint32_t lower, upper; int16_t weight; };
    struct Block { uint32_t base; uint8_t lower[8], upper[8]; int16_t weight[8]; bool vectorized; };
    struct Plan {
        std::vector<Axis> x, y;
        std::vector<ByteAxis> bytes;
        std::vector<Block> blocks;
        size_t begin = 0, end = 0;
    };
    Plan mToReference[2], mToSource[2];
    Frame mFrame;
    bool mConfigured = false;
    // Q7 bounds signed residuals to +/-32640; complementary Q16 vertical
    // weights then fit int32, with less than 1/256 LSB horizontal error.
    mutable std::vector<int16_t> mRows[2];
    static bool validRect(const Rect &r) {
        return std::isfinite(r.left) && std::isfinite(r.top) &&
                std::isfinite(r.width) && std::isfinite(r.height) &&
                r.left >= 0 && r.top >= 0 && r.width > 0 && r.height > 0 &&
                r.left + r.width <= 32768 && r.top + r.height <= 32768;
    }
    static bool sameRect(const Rect &a, const Rect &b) {
        return a.left == b.left && a.top == b.top && a.width == b.width && a.height == b.height;
    }
    static bool same(const Frame &a, const Frame &b) {
        return a.width == b.width && a.height == b.height &&
                sameRect(a.sensorWindow, b.sensorWindow) &&
                sameRect(a.referenceWindow, b.referenceWindow);
    }
    bool images(const Image &a, const Image &b) const {
        return mConfigured && QCamera3HdrAlgo::validImage(a) &&
                QCamera3HdrAlgo::validImage(b) && a.width == mFrame.width &&
                a.height == mFrame.height && b.width == a.width && b.height == a.height;
    }
    static void axes(std::vector<Axis> &axis, uint32_t count, double inputOrigin,
            double inputExtent, double outputOrigin, double outputExtent, bool restrict) {
        axis.resize(count);
        for (uint32_t p = 0; p < count; ++p) {
            const double sensor = outputOrigin + (p + 0.5) * outputExtent / count;
            const bool active = !restrict || (sensor >= inputOrigin && sensor < inputOrigin + inputExtent);
            const double sample = std::max(0.0, std::min(double(count - 1),
                    (sensor - inputOrigin) * count / inputExtent - 0.5));
            const auto lower = uint32_t(sample);
            axis[p] = {lower, std::min(lower + 1, count - 1),
                uint32_t(std::lround((sample - lower) * 65536.0)), active};
        }
    }
    static void blocks(Plan &plan, uint32_t width, unsigned unit) {
        plan.bytes.resize(width);
        for (uint32_t x = 0; x < width; ++x) {
            const auto &axis = plan.x[x / unit];
            const uint32_t lower = axis.weight == 65536 ? axis.upper : axis.lower;
            const int32_t weight = axis.weight < 32768 ? int32_t(axis.weight) : int32_t(axis.weight) - 65536;
            plan.bytes[x] = {lower * unit + x % unit, axis.upper * unit + x % unit, int16_t(weight)};
        }
        plan.blocks.resize(width / 8);
        for (uint32_t i = 0; i < plan.blocks.size(); ++i) {
            auto &block = plan.blocks[i];
            const uint32_t start = plan.bytes[i * 8].lower;
            block.base = width >= 16 && start > width - 16 ? width - 16 : start;
            block.vectorized = width >= 16;
            for (unsigned j = 0; j < 8; ++j) {
                const auto &axis = plan.bytes[i * 8 + j];
                if (axis.lower < block.base || axis.upper < block.base ||
                        axis.lower - block.base >= 16 || axis.upper - block.base >= 16)
                    block.vectorized = false;
                block.lower[j] = axis.lower - block.base;
                block.upper[j] = axis.upper - block.base;
                block.weight[j] = axis.weight;
            }
        }
    }
    unsigned cachedRow(int64_t cached[2], uint32_t y, uint32_t avoid,
            const Image &after, const Image *before, unsigned plane, const Plan &plan) const {
        for (unsigned i = 0; i < 2; ++i) if (cached[i] == y) return i;
        const unsigned slot = cached[0] == avoid ? 1 : 0;
        auto &out = mRows[slot];
        const auto *row = after.data + (plane ? after.uvOffset : after.yOffset) +
                size_t(y) * (plane ? after.uvStride : after.yStride);
        const auto *base = before ? before->data + (plane ? before->uvOffset : before->yOffset) +
                size_t(y) * (plane ? before->uvStride : before->yStride) : nullptr;
        size_t x = 0;
        for (const auto &block : plan.blocks) {
            (void)block;
#if defined(__ARM_NEON)
            if (block.vectorized) {
                const uint8x8x2_t afterTable = {{vld1_u8(row + block.base), vld1_u8(row + block.base + 8)}};
                int16x8_t a = vreinterpretq_s16_u16(vmovl_u8(vtbl2_u8(afterTable, vld1_u8(block.lower))));
                int16x8_t b = vreinterpretq_s16_u16(vmovl_u8(vtbl2_u8(afterTable, vld1_u8(block.upper))));
                if (base) {
                    const uint8x8x2_t beforeTable = {{vld1_u8(base + block.base), vld1_u8(base + block.base + 8)}};
                    a = vsubq_s16(a, vreinterpretq_s16_u16(vmovl_u8(vtbl2_u8(beforeTable, vld1_u8(block.lower)))));
                    b = vsubq_s16(b, vreinterpretq_s16_u16(vmovl_u8(vtbl2_u8(beforeTable, vld1_u8(block.upper)))));
                }
                const int16x8_t weight = vld1q_s16(block.weight);
                vst1q_s16(out.data() + x, horizontal(a, b, weight));
            } else
#endif
            {
                for (unsigned j = 0; j < 8; ++j) {
                    const auto &axis = plan.bytes[x + j];
                    const int32_t a = int(row[axis.lower]) - (base ? base[axis.lower] : 0);
                    const int32_t b = int(row[axis.upper]) - (base ? base[axis.upper] : 0);
                    out[x + j] = horizontal(a, b, axis.weight);
                }
            }
            x += 8;
        }
        for (; x < plan.bytes.size(); ++x) {
            const auto &axis = plan.bytes[x];
            const int32_t a = int(row[axis.lower]) - (base ? base[axis.lower] : 0);
            const int32_t b = int(row[axis.upper]) - (base ? base[axis.upper] : 0);
            out[x] = horizontal(a, b, axis.weight);
        }
        cached[slot] = y;
        return slot;
    }
    static int16_t horizontal(int a, int b, int16_t weight) {
        // Fold Q16 around the nearest endpoint. Even signed residual differences
        // fit Q6 int16, so NEON rounded high multiply preserves the Q7 result.
        return (weight < 0 ? b : a) * 128 + floorShift((b - a) * weight + 256, 9);
    }
#if defined(__ARM_NEON)
    static int16x8_t horizontal(int16x8_t a, int16x8_t b, int16x8_t weight) {
        const int16x8_t nearest = vbslq_s16(vcltq_s16(weight, vdupq_n_s16(0)), b, a);
        return vaddq_s16(vshlq_n_s16(nearest, 7),
                vqrdmulhq_s16(vshlq_n_s16(vsubq_s16(b, a), 6), weight));
    }
#endif
    void vertical(const std::vector<int16_t> &top, const std::vector<int16_t> &bottom,
            uint32_t weight, size_t begin, size_t end,
            const uint8_t *original, uint8_t *output, bool *changed) const {
        size_t x = begin;
#if defined(__ARM_NEON)
        const int32x4_t w = vdupq_n_s32(weight), inverse = vdupq_n_s32(65536 - weight);
        for (; x + 8 <= end; x += 8) {
            const int16x8_t a = vld1q_s16(top.data() + x), b = vld1q_s16(bottom.data() + x);
            const int32x4_t lo = vmlaq_s32(vmulq_s32(vmovl_s16(vget_low_s16(a)), inverse),
                    vmovl_s16(vget_low_s16(b)), w);
            const int32x4_t hi = vmlaq_s32(vmulq_s32(vmovl_s16(vget_high_s16(a)), inverse),
                    vmovl_s16(vget_high_s16(b)), w);
            int16x8_t values = vcombine_s16(vmovn_s32(vrshrq_n_s32(lo, 23)), vmovn_s32(vrshrq_n_s32(hi, 23)));
            if (original) values = vaddq_s16(values, vreinterpretq_s16_u16(vmovl_u8(vld1_u8(original + x))));
            const uint8x8_t bytes = vqmovun_s16(values);
            vst1_u8(output + x, bytes);
            if (changed && !*changed && vget_lane_u64(vreinterpret_u64_u8(vceq_u8(bytes, vld1_u8(original + x))), 0) != UINT64_MAX)
                *changed = true;
        }
#endif
        for (; x < end; ++x) {
            const int32_t delta = floorShift(top[x] * int32_t(65536 - weight) +
                    bottom[x] * int32_t(weight) + (1 << 22), 23);
            output[x] = std::max(0, std::min(255, delta + (original ? int(original[x]) : 0)));
            if (changed) *changed |= output[x] != original[x];
        }
    }
    static int32_t floorShift(int32_t value, unsigned shift) {
        return value >= 0 ? value >> shift : -int32_t((uint32_t(-value) + (1U << shift) - 1) >> shift);
    }
};
} // namespace qcamera
#endif
