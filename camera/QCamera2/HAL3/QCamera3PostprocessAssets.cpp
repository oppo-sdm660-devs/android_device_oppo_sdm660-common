/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3PostprocessAssets.h"

#include <cmath>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

namespace qcamera {
namespace {

using Asset = QCamera3PostprocessAssets::Asset;
using Image = QCamera3PostprocessAssets::Image;
using Options = QCamera3PostprocessAssets::Options;
using Rect = QCamera3PostprocessAssets::Rect;
using Result = QCamera3PostprocessAssets::Result;

constexpr uint32_t kMaxImageSide = QCamera3PostprocessAssets::MAX_IMAGE_SIDE;
constexpr uint32_t kMaxImagePixels = QCamera3PostprocessAssets::MAX_IMAGE_PIXELS;
constexpr uint32_t kMaxScratchBytes = QCamera3PostprocessAssets::MAX_SCRATCH_BYTES;
constexpr uint32_t kMaxAssetSide = QCamera3PostprocessAssets::MAX_ASSET_SIDE;
constexpr uint32_t kMaxScale = QCamera3PostprocessAssets::MAX_SCALE;
constexpr uint32_t kMaxSourceName = QCamera3PostprocessAssets::MAX_SOURCE_NAME;
constexpr uint32_t kMaxPathBytes = QCamera3PostprocessAssets::MAX_PATH_BYTES;
constexpr const char *kInstalledRoot = "/vendor/etc/camera/postprocess";

bool stopped(const std::atomic<bool> *flag) { return flag && flag->load(); }

bool validImage(const Image &image) {
    return image.width >= 2 && image.height >= 2 &&
            image.width <= kMaxImageSide && image.height <= kMaxImageSide &&
            uint64_t(image.width) * image.height <= kMaxImagePixels &&
            QCamera3HdrAlgo::validImage(image);
}

uint8_t clampByte(int32_t value) {
    if (value < 0) return 0;
    if (value > 255) return 255;
    return static_cast<uint8_t>(value);
}

// Floor division by 256 for negative numerators (>> on negatives is
// implementation-defined before C++20).
int32_t floorDiv256(int32_t value) {
    return value >= 0 ? (value >> 8) : -(((-value) + 255) >> 8);
}

void argbToYuv(uint32_t argb, int32_t &y, int32_t &cb, int32_t &cr) {
    const int32_t r = static_cast<int32_t>((argb >> 16) & 0xFFU);
    const int32_t g = static_cast<int32_t>((argb >> 8) & 0xFFU);
    const int32_t b = static_cast<int32_t>(argb & 0xFFU);
    y = clampByte(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
    cb = clampByte(floorDiv256(-38 * r - 74 * g + 112 * b + 128) + 128);
    cr = clampByte(floorDiv256(112 * r - 94 * g - 18 * b + 128) + 128);
}

// BT.601 limited-range inverse transform for opaque NV21 assets.
void yuvToRgb(int32_t y, int32_t cb, int32_t cr, uint32_t &r, uint32_t &g, uint32_t &b) {
    const int32_t c = y - 16;
    const int32_t d = cb - 128;
    const int32_t e = cr - 128;
    r = clampByte((298 * c + 409 * e + 128) >> 8);
    g = clampByte(floorDiv256(298 * c - 100 * d - 208 * e + 128));
    b = clampByte((298 * c + 516 * d + 128) >> 8);
}

struct Texel {
    uint32_t a, r, g, b;
};

Texel fetchTexel(const Asset &asset, uint32_t x, uint32_t y) {
    const auto *bytes = static_cast<const uint8_t *>(asset.pixels);
    if (asset.format == QCamera3PostprocessAssets::FORMAT_ARGB32) {
        const uint8_t *pixel = bytes + (size_t(y) * asset.stride + x) * 4;
        return {pixel[3], pixel[2], pixel[1], pixel[0]};
    }
    const uint8_t *base = bytes + size_t(y) * asset.stride + x;
    const uint8_t *uv = bytes + size_t(asset.stride) * asset.height +
            size_t(y / 2) * asset.stride + size_t(x / 2) * 2;
    Texel texel = {};
    texel.a = 255; // NV21 assets are opaque; use ARGB32 for transparency.
    yuvToRgb(*base, uv[1], uv[0], texel.r, texel.g, texel.b);
    return texel;
}

uint32_t interpolate(uint32_t t00, uint32_t t01, uint32_t t10, uint32_t t11,
        uint32_t w00, uint32_t w01, uint32_t w10, uint32_t w11) {
    const uint32_t value = (t00 * w00 + t01 * w01 + t10 * w10 + t11 * w11 + 16384U) >> 15;
    return value > 255U ? 255U : value;
}

void sampleAsset(const Asset &asset, int64_t uQ16, int64_t vQ16, Texel &out) {
    const int64_t maxU = (int64_t(asset.width) - 1) << 16;
    const int64_t maxV = (int64_t(asset.height) - 1) << 16;
    if (uQ16 < 0) uQ16 = 0;
    if (uQ16 > maxU) uQ16 = maxU;
    if (vQ16 < 0) vQ16 = 0;
    if (vQ16 > maxV) vQ16 = maxV;
    const uint32_t x0 = static_cast<uint32_t>(uQ16 >> 16);
    const uint32_t y0 = static_cast<uint32_t>(vQ16 >> 16);
    const uint32_t x1 = x0 + 1 < asset.width ? x0 + 1 : x0;
    const uint32_t y1 = y0 + 1 < asset.height ? y0 + 1 : y0;
    const uint32_t fx = static_cast<uint32_t>(uQ16 & 0xFFFF) >> 1; // Q15
    const uint32_t fy = static_cast<uint32_t>(vQ16 & 0xFFFF) >> 1;
    const uint32_t w00 = ((32768U - fx) * (32768U - fy)) >> 15;
    const uint32_t w01 = (fx * (32768U - fy)) >> 15;
    const uint32_t w10 = ((32768U - fx) * fy) >> 15;
    const uint32_t w11 = (fx * fy) >> 15;
    const Texel t00 = fetchTexel(asset, x0, y0);
    const Texel t01 = fetchTexel(asset, x1, y0);
    const Texel t10 = fetchTexel(asset, x0, y1);
    const Texel t11 = fetchTexel(asset, x1, y1);
    out.a = interpolate(t00.a, t01.a, t10.a, t11.a, w00, w01, w10, w11);
    out.r = interpolate(t00.r, t01.r, t10.r, t11.r, w00, w01, w10, w11);
    out.g = interpolate(t00.g, t01.g, t10.g, t11.g, w00, w01, w10, w11);
    out.b = interpolate(t00.b, t01.b, t10.b, t11.b, w00, w01, w10, w11);
}

uint8_t blendChannel(uint8_t destination, int32_t source, uint32_t alpha) {
    return static_cast<uint8_t>((source * static_cast<int32_t>(alpha) +
            static_cast<int32_t>(destination) * static_cast<int32_t>(255U - alpha) + 127) / 255);
}

int32_t checkScale(const Asset &asset, uint32_t spanX, uint32_t spanY) {
    if (!spanX || !spanY) return -EINVAL;
    if (uint64_t(asset.width) > uint64_t(spanX) * kMaxScale ||
            uint64_t(spanX) > uint64_t(asset.width) * kMaxScale) return -ERANGE;
    if (uint64_t(asset.height) > uint64_t(spanY) * kMaxScale ||
            uint64_t(spanY) > uint64_t(asset.height) * kMaxScale) return -ERANGE;
    return 0;
}

// Packed scratch frame: Y at 0 (stride = width), UV at width*height
// (stride = width). The bounded block arrays live in the tail region.
struct Scratch {
    uint8_t *bytes = nullptr;
    uint8_t *y = nullptr;
    uint8_t *uv = nullptr;
    uint16_t *blockSum = nullptr;
    uint16_t *blockCount = nullptr;
    uint32_t *blockSource = nullptr;
    ~Scratch() { free(bytes); }
};

void copyOrMirror(const Image &source, const Scratch &scratch, bool mirror) {
    const uint8_t *sourceY = source.data + source.yOffset;
    const uint8_t *sourceUv = source.data + source.uvOffset;
    for (uint32_t row = 0; row < source.height; ++row) {
        const uint8_t *from = sourceY + size_t(row) * source.yStride;
        uint8_t *to = scratch.y + size_t(row) * source.width;
        if (!mirror) {
            memcpy(to, from, source.width);
        } else {
            for (uint32_t x = 0; x < source.width; ++x) to[x] = from[source.width - 1 - x];
        }
    }
    const uint32_t blocks = source.width / 2;
    for (uint32_t row = 0; row < source.height / 2; ++row) {
        const uint8_t *from = sourceUv + size_t(row) * source.uvStride;
        uint8_t *to = scratch.uv + size_t(row) * source.width;
        for (uint32_t block = 0; block < blocks; ++block) {
            // One chroma sample is the V/U byte pair; a horizontal mirror
            // reverses the sample order and never swaps V with U.
            const uint8_t *pair = from + size_t(mirror ? blocks - 1 - block : block) * 2;
            to[size_t(block) * 2] = pair[0];
            to[size_t(block) * 2 + 1] = pair[1];
        }
    }
}

void commit(const Scratch &scratch, const Image &destination) {
    uint8_t *destinationY = destination.data + destination.yOffset;
    uint8_t *destinationUv = destination.data + destination.uvOffset;
    for (uint32_t row = 0; row < destination.height; ++row) {
        memcpy(destinationY + size_t(row) * destination.yStride,
                scratch.y + size_t(row) * destination.width, destination.width);
    }
    for (uint32_t row = 0; row < destination.height / 2; ++row) {
        memcpy(destinationUv + size_t(row) * destination.uvStride,
                scratch.uv + size_t(row) * destination.width, destination.width);
    }
}

void flushChromaRow(const Scratch &scratch, uint32_t chromaRow, uint32_t blocks) {
    uint8_t *row = scratch.uv + size_t(chromaRow) * blocks * 2;
    for (uint32_t block = 0; block < blocks; ++block) {
        const uint32_t count = scratch.blockCount[block];
        if (!count) continue;
        const uint32_t alpha = (scratch.blockSum[block] + count / 2) / count;
        if (!alpha) continue;
        int32_t y = 0, cb = 0, cr = 0;
        argbToYuv(scratch.blockSource[block], y, cb, cr);
        uint8_t *sample = row + size_t(block) * 2;
        sample[0] = blendChannel(sample[0], cr, alpha); // V
        sample[1] = blendChannel(sample[1], cb, alpha); // U
    }
}

// Composites one asset into the packed scratch frame. The span and the asset
// were validated by the caller, so only cancellation can fail here.
int32_t compositeAsset(const Scratch &scratch, uint32_t width, uint32_t height,
        uint32_t left, uint32_t top, uint32_t right, uint32_t bottom,
        const Asset &asset, uint32_t opacity, uint32_t &written, uint32_t &skipped,
        const std::atomic<bool> *cancelled) {
    const uint32_t spanX = right - left;
    const uint32_t spanY = bottom - top;
    const uint32_t blocks = width / 2;
    if (!spanX || !spanY || right > width || bottom > height) return -EINVAL;
    for (uint32_t y = top; y < bottom; ++y) {
        if (((y - top) % QCamera3PostprocessAssets::CANCEL_ROWS) == 0 && stopped(cancelled))
            return -ECANCELED;
        if ((y & 1U) == 0 || y == top) {
            memset(scratch.blockSum, 0, size_t(blocks) * sizeof(uint16_t));
            memset(scratch.blockCount, 0, size_t(blocks) * sizeof(uint16_t));
        }
        const int64_t vQ16 = (int64_t(2 * (y - top) + 1) * asset.height * 32768) /
                int64_t(spanY) - 32768;
        uint8_t *rowY = scratch.y + size_t(y) * width;
        for (uint32_t x = left; x < right; ++x) {
            const int64_t uQ16 = (int64_t(2 * (x - left) + 1) * asset.width * 32768) /
                    int64_t(spanX) - 32768;
            Texel texel = {};
            sampleAsset(asset, uQ16, vQ16, texel);
            const uint32_t alpha = (texel.a * opacity + 127U) / 255U;
            if (!alpha) {
                ++skipped;
                continue;
            }
            const uint32_t block = x >> 1;
            if (scratch.blockCount[block] == 0)
                scratch.blockSource[block] = (texel.a << 24) | (texel.r << 16) |
                        (texel.g << 8) | texel.b;
            scratch.blockSum[block] = static_cast<uint16_t>(scratch.blockSum[block] + alpha);
            scratch.blockCount[block] = static_cast<uint16_t>(scratch.blockCount[block] + 1);
            int32_t luma = 0, cb = 0, cr = 0;
            argbToYuv((texel.r << 16) | (texel.g << 8) | texel.b, luma, cb, cr);
            rowY[x] = blendChannel(rowY[x], luma, alpha);
            ++written;
        }
        if ((y & 1U) || y + 1 == bottom) flushChromaRow(scratch, y / 2, blocks);
    }
    return 0;
}

bool unusedRect(const Rect &rect) {
    return (rect.left == 0.0f && rect.top == 0.0f && rect.right == 0.0f &&
            rect.bottom == 0.0f) || QCamera3PostprocessAssets::validRect(rect);
}

bool validId(const char *begin, size_t length) {
    if (!length || length > 64) return false;
    for (size_t i = 0; i < length; ++i) {
        const char c = begin[i];
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        if (!allowed) return false;
    }
    return true;
}

bool parseDimension(const char *begin, const char *end, uint32_t &value) {
    if (begin == end) return false;
    if (end - begin > 1 && *begin == '0') return false;
    uint32_t parsed = 0;
    for (const char *p = begin; p < end; ++p) {
        if (*p < '0' || *p > '9') return false;
        parsed = parsed * 10U + static_cast<uint32_t>(*p - '0');
        if (parsed > kMaxAssetSide) return false;
    }
    if (!parsed) return false;
    value = parsed;
    return true;
}

bool containsDotDot(const char *path) {
    for (const char *p = path; p[0] && p[1]; ++p) {
        if (p[0] == '.' && p[1] == '.') return true;
    }
    return false;
}

size_t rootLength(const char *root) {
    size_t length = strlen(root);
    while (length > 1 && root[length - 1] == '/') --length;
    return length;
}

} // namespace

bool QCamera3PostprocessAssets::Options::valid(const Options &o) {
    if ((o.enable != 0 && o.enable != 1) || (o.mirror != 0 && o.mirror != 1) ||
            o.opacity < 0 || o.opacity > 255) return false;
    if (o.enable == 0) {
        // Nothing is requested: assets and rectangles are not inspected.
        return true;
    }
    if (o.watermark) {
        if (!validRect(o.watermarkRect) || !validAsset(*o.watermark)) return false;
    } else if (!unusedRect(o.watermarkRect)) {
        return false;
    }
    if (o.sticker) {
        if (!validRect(o.stickerRect) || !validAsset(*o.sticker)) return false;
    } else if (!unusedRect(o.stickerRect)) {
        return false;
    }
    return true;
}

bool QCamera3PostprocessAssets::validRect(const Rect &rect) {
    if (!std::isfinite(rect.left) || !std::isfinite(rect.top) ||
            !std::isfinite(rect.right) || !std::isfinite(rect.bottom)) return false;
    return rect.left >= 0.0f && rect.top >= 0.0f && rect.right <= 1.0f &&
            rect.bottom <= 1.0f && rect.left < rect.right && rect.top < rect.bottom;
}

bool QCamera3PostprocessAssets::validAsset(const Asset &asset) {
    if (!asset.pixels || !asset.width || !asset.height ||
            asset.width > kMaxAssetSide || asset.height > kMaxAssetSide) return false;
    if (asset.stride < asset.width || asset.stride > kMaxAssetSide) return false;
    if (asset.format == FORMAT_ARGB32) {
        const uint64_t needed = uint64_t(asset.stride) * asset.height * 4U;
        return needed <= MAX_ASSET_BYTES && asset.length >= needed;
    }
    if (asset.format == FORMAT_NV21) {
        if ((asset.width & 1U) || (asset.height & 1U)) return false;
        const uint64_t needed = uint64_t(asset.stride) * asset.height +
                uint64_t(asset.stride) * (asset.height / 2);
        return needed <= MAX_ASSET_BYTES && asset.length >= needed;
    }
    return false;
}

int32_t QCamera3PostprocessAssets::resolveRect(const Rect &normalized, uint32_t extentX,
        uint32_t extentY, uint32_t &left, uint32_t &top, uint32_t &right, uint32_t &bottom) {
    left = top = right = bottom = 0;
    if (!validRect(normalized) || !extentX || !extentY ||
            extentX > kMaxImageSide || extentY > kMaxImageSide) return -EINVAL;
    const auto roundExtent = [](float value, uint32_t extent) -> uint32_t {
        const double scaled = std::floor(static_cast<double>(value) * extent + 0.5);
        if (scaled <= 0.0) return 0;
        if (scaled >= static_cast<double>(extent)) return extent;
        return static_cast<uint32_t>(scaled);
    };
    left = roundExtent(normalized.left, extentX);
    right = roundExtent(normalized.right, extentX);
    top = roundExtent(normalized.top, extentY);
    bottom = roundExtent(normalized.bottom, extentY);
    if (right <= left || bottom <= top) return -EINVAL;
    return 0;
}

int32_t QCamera3PostprocessAssets::mapRectForJpeg(const Rect &viewed, int32_t rotationDegrees,
        Rect &mapped) {
    if (!validRect(viewed)) return -EINVAL;
    Rect result = {};
    switch (rotationDegrees) {
    case JPEG_ROTATION_0:
        result = viewed;
        break;
    case JPEG_ROTATION_90:
        // viewed = stored rotated 90 degrees clockwise: (u, v) -> (1 - v, u).
        result = {viewed.top, 1.0f - viewed.right, viewed.bottom, 1.0f - viewed.left};
        break;
    case JPEG_ROTATION_180:
        result = {1.0f - viewed.right, 1.0f - viewed.bottom,
                1.0f - viewed.left, 1.0f - viewed.top};
        break;
    case JPEG_ROTATION_270:
        // viewed = stored rotated 270 degrees clockwise: (u, v) -> (v, 1 - u).
        result = {1.0f - viewed.bottom, viewed.left, 1.0f - viewed.top, viewed.right};
        break;
    default:
        return -EINVAL;
    }
    if (!validRect(result)) return -ERANGE;
    mapped = result;
    return 0;
}

const char *QCamera3PostprocessAssets::name() { return "PostprocessAssets"; }

bool QCamera3PostprocessAssets::isAvailable() { return true; }

bool QCamera3PostprocessAssets::supports(uint32_t width, uint32_t height) {
    return width >= 2 && height >= 2 && width <= kMaxImageSide && height <= kMaxImageSide &&
            !(width & 1U) && !(height & 1U) &&
            uint64_t(width) * height <= kMaxImagePixels;
}

bool QCamera3PostprocessAssets::validOptions(const Options &options) {
    return Options::valid(options);
}

const char *QCamera3PostprocessAssets::installedRoot() { return kInstalledRoot; }

int32_t QCamera3PostprocessAssets::parseAssetName(const char *name, uint32_t &width,
        uint32_t &height, int32_t &format) {
    width = height = 0;
    format = 0;
    if (!name || !name[0]) return -EINVAL;
    const size_t length = strnlen(name, kMaxSourceName + 1);
    if (!length || length > kMaxSourceName) return -EINVAL;
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name) return -EINVAL;
    if (!strcmp(dot, ".argb")) format = FORMAT_ARGB32;
    else if (!strcmp(dot, ".nv21") || !strcmp(dot, ".yuv")) format = FORMAT_NV21;
    else return -EPROTONOSUPPORT;
    const char *underscore = nullptr;
    for (const char *p = dot - 1; p > name; --p) {
        if (*p == '_') { underscore = p; break; }
    }
    if (!underscore || !validId(name, static_cast<size_t>(underscore - name))) return -EINVAL;
    const char *dimensions = underscore + 1;
    const char *separator = nullptr;
    for (const char *p = dimensions; p < dot; ++p) {
        if (*p == 'x') { separator = p; break; }
    }
    if (!separator || !parseDimension(dimensions, separator, width) ||
            !parseDimension(separator + 1, dot, height)) return -EINVAL;
    return 0;
}

int32_t QCamera3PostprocessAssets::loadAssetAt(const char *root, const char *source,
        uint8_t *storage, size_t capacity, size_t &length, Asset &out) {
    length = 0;
    out = {};
    if (!root || !source || !storage || !capacity) return -EINVAL;
    const size_t rootSize = rootLength(root);
    if (!rootSize || root[0] != '/' || rootSize > kMaxSourceName ||
            containsDotDot(root)) return -EINVAL;
    const size_t sourceSize = strnlen(source, kMaxPathBytes + 1);
    if (!sourceSize || sourceSize > kMaxSourceName || containsDotDot(source)) return -EINVAL;
    char path[kMaxPathBytes + 1] = {};
    if (source[0] == '/') {
        // An absolute path is accepted only when it really lives in the root.
        if (sourceSize <= rootSize || strncmp(source, root, rootSize) ||
                source[rootSize] != '/') return -EINVAL;
        if (sourceSize + 1 > sizeof(path)) return -E2BIG;
        memcpy(path, source, sourceSize + 1);
    } else {
        if (strchr(source, '/') || !validId(source, sourceSize)) return -EINVAL;
        if (rootSize + 1 + sourceSize + 1 > sizeof(path)) return -E2BIG;
        memcpy(path, root, rootSize);
        path[rootSize] = '/';
        memcpy(path + rootSize + 1, source, sourceSize + 1);
    }
    const char *fileName = strrchr(path, '/') + 1;
    uint32_t width = 0, height = 0;
    int32_t format = 0;
    int32_t status = parseAssetName(fileName, width, height, format);
    if (status) return status;
    const uint64_t expected = format == FORMAT_ARGB32 ? uint64_t(width) * height * 4U
            : uint64_t(width) * height * 3U / 2U;
    if (expected > MAX_ASSET_BYTES) return -E2BIG;
    const int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return -errno;
    struct stat info;
    if (fstat(fd, &info)) {
        const int error = errno;
        close(fd);
        return -error;
    }
    if (!S_ISREG(info.st_mode)) { close(fd); return -EINVAL; }
    if (info.st_size <= 0) { close(fd); return -EINVAL; }
    if (uint64_t(info.st_size) > MAX_ASSET_BYTES) { close(fd); return -E2BIG; }
    if (uint64_t(info.st_size) != expected) { close(fd); return -EMSGSIZE; }
    if (uint64_t(info.st_size) > capacity) { close(fd); return -E2BIG; }
    size_t total = 0;
    while (total < expected) {
        const ssize_t got = read(fd, storage + total, size_t(expected) - total);
        if (got < 0) {
            if (errno == EINTR) continue;
            const int error = errno;
            close(fd);
            return -error;
        }
        if (!got) { close(fd); return -EIO; }
        total += static_cast<size_t>(got);
    }
    close(fd);
    length = total;
    out.pixels = storage;
    out.length = total;
    out.width = width;
    out.height = height;
    out.stride = width;
    out.format = format;
    return 0;
}

int32_t QCamera3PostprocessAssets::loadAsset(const char *source, uint8_t *storage,
        size_t capacity, size_t &length, Asset &out) {
    return loadAssetAt(kInstalledRoot, source, storage, capacity, length, out);
}

int32_t QCamera3PostprocessAssets::process(const Image &source, const Image &destination,
        const Options &options, Result &result, const std::atomic<bool> *cancelled) {
    result = {};
    result.frameNumber = options.frameNumber;
    if (!validOptions(options)) {
        result.status = -EINVAL;
        return -EINVAL;
    }
    if (!validImage(source) || !validImage(destination) ||
            source.width != destination.width || source.height != destination.height) {
        result.status = -EINVAL;
        return -EINVAL;
    }
    const uint32_t width = source.width;
    const uint32_t height = source.height;
    const bool watermarkVisible = options.watermark != nullptr && options.opacity > 0;
    const bool stickerVisible = options.sticker != nullptr && options.opacity > 0;
    if (options.enable == 0 ||
            (options.mirror == 0 && !watermarkVisible && !stickerVisible)) {
        // Strict bypass: the destination keeps its previous content.
        result.bypassed = 1;
        return 0;
    }

    // Resolve and bound every placement before the first write.
    uint32_t watermarkSpan[4] = {};
    uint32_t stickerSpan[4] = {};
    if (watermarkVisible) {
        int32_t status = resolveRect(options.watermarkRect, width, height,
                watermarkSpan[0], watermarkSpan[1], watermarkSpan[2], watermarkSpan[3]);
        if (status) { result.status = status; return status; }
        status = checkScale(*options.watermark, watermarkSpan[2] - watermarkSpan[0],
                watermarkSpan[3] - watermarkSpan[1]);
        if (status) { result.status = status; return status; }
    }
    if (stickerVisible) {
        int32_t status = resolveRect(options.stickerRect, width, height,
                stickerSpan[0], stickerSpan[1], stickerSpan[2], stickerSpan[3]);
        if (status) { result.status = status; return status; }
        status = checkScale(*options.sticker, stickerSpan[2] - stickerSpan[0],
                stickerSpan[3] - stickerSpan[1]);
        if (status) { result.status = status; return status; }
    }

    // One documented bounded scratch frame plus its block arrays. The tail is
    // 4-byte aligned so the block arrays are naturally aligned for every even
    // frame geometry.
    const uint64_t pixels = uint64_t(width) * height;
    const uint64_t frameBytes = pixels + pixels / 2U;
    const uint64_t tailOffset = (frameBytes + 3U) & ~uint64_t(3U);
    const uint64_t tailBytes = 8U * (width / 2U);
    if (tailOffset + tailBytes > kMaxScratchBytes) {
        result.status = -E2BIG;
        return -E2BIG;
    }
    Scratch scratch;
    scratch.bytes = static_cast<uint8_t *>(malloc(size_t(tailOffset + tailBytes)));
    if (!scratch.bytes) {
        result.status = -ENOMEM;
        return -ENOMEM;
    }
    scratch.y = scratch.bytes;
    scratch.uv = scratch.bytes + pixels;
    uint8_t *tail = scratch.bytes + tailOffset;
    scratch.blockSum = reinterpret_cast<uint16_t *>(tail);
    scratch.blockCount = scratch.blockSum + width / 2U;
    scratch.blockSource = reinterpret_cast<uint32_t *>(scratch.blockCount + width / 2U);
    if (stopped(cancelled)) {
        result.status = -ECANCELED;
        return -ECANCELED;
    }
    copyOrMirror(source, scratch, options.mirror != 0);

    uint32_t watermarkPixels = 0, stickerPixels = 0, skipped = 0;
    if (watermarkVisible) {
        const int32_t status = compositeAsset(scratch, width, height, watermarkSpan[0],
                watermarkSpan[1], watermarkSpan[2], watermarkSpan[3], *options.watermark,
                static_cast<uint32_t>(options.opacity), watermarkPixels, skipped, cancelled);
        if (status) { result.status = status; return status; }
    }
    if (stickerVisible) {
        const int32_t status = compositeAsset(scratch, width, height, stickerSpan[0],
                stickerSpan[1], stickerSpan[2], stickerSpan[3], *options.sticker,
                static_cast<uint32_t>(options.opacity), stickerPixels, skipped, cancelled);
        if (status) { result.status = status; return status; }
    }
    if (stopped(cancelled)) {
        result.status = -ECANCELED;
        return -ECANCELED;
    }
    const bool changed = options.mirror != 0 || watermarkPixels > 0 || stickerPixels > 0;
    if (changed) commit(scratch, destination);
    result.status = 0;
    result.applied = changed ? 1 : 0;
    result.watermarkPixels = watermarkPixels;
    result.stickerPixels = stickerPixels;
    result.mirrored = options.mirror != 0 ? 1 : 0;
    result.detail[0] = static_cast<int32_t>(watermarkPixels);
    result.detail[1] = static_cast<int32_t>(stickerPixels);
    result.detail[2] = result.mirrored;
    result.detail[3] = static_cast<int32_t>(skipped);
    return 0;
}

int32_t QCamera3PostprocessAssets::processJpegEdit(const Image &source,
        const Image &destination, int32_t rotationDegrees, const Options &options,
        Result &result, const std::atomic<bool> *cancelled) {
    Options mapped = options;
    if (options.enable != 0) {
        // Rectangles arrive in the viewed orientation; the edit surface is
        // unrotated, so map them back before compositing.
        int32_t status = mapRectForJpeg(options.watermarkRect, rotationDegrees,
                mapped.watermarkRect);
        if (status && options.watermark) {
            result = {};
            result.frameNumber = options.frameNumber;
            result.status = status;
            return status;
        }
        status = mapRectForJpeg(options.stickerRect, rotationDegrees, mapped.stickerRect);
        if (status && options.sticker) {
            result = {};
            result.frameNumber = options.frameNumber;
            result.status = status;
            return status;
        }
    }
    return process(source, destination, mapped, result, cancelled);
}

} // namespace qcamera
