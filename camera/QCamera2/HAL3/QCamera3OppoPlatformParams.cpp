/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3OppoPlatformParams.h"

#include <string.h>

namespace qcamera {
namespace oppo_platform {

uint64_t maskForStillMode(int32_t stillMode)
{
    /* Values mirror the org.lineageos native still mode enum. */
    switch (stillMode) {
    case 1: return kAlgoHdr;              /* HDR */
    case 2: return kAlgoLlhdr;            /* LLHDR / low light */
    case 3: return kAlgoPicBest;          /* MFNR: original block reuses picbest */
    case 4: return kAlgoSuperPhoto;       /* super photo / high resolution */
    case 5: return kAlgoFaceBeauty | kAlgoFaceInfo; /* rear beauty + Venus FD */
    case 7: return kAlgoBokeh;            /* dual-camera bokeh */
    case 9: return kAlgoVdSces;           /* AI scene / SCES */
    case 10: return kAlgoArcFaceBeauty | kAlgoFaceInfo; /* front beauty (ARC) + faceInfo */
    default: return 0;
    }
}

bool supportedMask(uint64_t mask)
{
    const uint64_t known = kAlgoBlurless | kAlgoSuperPhoto | kAlgoStFaceBlur | kAlgoFilter |
            kAlgoFaceInfo | kAlgoFaceBeauty | kAlgoBokeh | kAlgoHdr | kAlgoArcFaceBeauty |
            kAlgoVdSces | kAlgoAsd | kAlgoPicBest | kAlgoImageAdjust | kAlgoPortraitLight |
            kAlgoLlhdr | kAlgoSlam;
    const uint64_t algorithms = mask & ~kInProcessBackend;
    return algorithms != 0 && (algorithms & ~known) == 0;
}

bool isPlatformStillMode(int32_t stillMode)
{
    const uint64_t mask = maskForStillMode(stillMode);
    return mask != 0 && (mask & ~kSupportedPlatformMask) == 0;
}

const char *algorithmName(uint64_t bit)
{
    switch (bit) {
    case kAlgoBlurless: return "Blurless";
    case kAlgoSuperPhoto: return "SuperPhoto";
    case kAlgoStFaceBlur: return "StFaceBlur";
    case kAlgoFilter: return "Filter";
    case kAlgoFaceInfo: return "FaceInfo";
    case kAlgoFaceBeauty: return "FaceBeauty";
    case kAlgoBokeh: return "Bokeh";
    case kAlgoHdr: return "HDR";
    case kAlgoArcFaceBeauty: return "ArcFaceBeauty";
    case kAlgoVdSces: return "VDSces";
    case kAlgoAsd: return "ASD";
    case kAlgoPicBest: return "PicBest";
    case kAlgoImageAdjust: return "ImageAdjust";
    case kAlgoPortraitLight: return "PortraitLight";
    case kAlgoLlhdr: return "LLHDR";
    case kAlgoSlam: return "Slam";
    default: return "Unknown";
    }
}

uint32_t readU32(const uint8_t *base, size_t offset)
{
    uint32_t value = 0;
    memcpy(&value, base + offset, sizeof(value));
    return value;
}

void writeU32(uint8_t *base, size_t offset, uint32_t value)
{
    memcpy(base + offset, &value, sizeof(value));
}

uint8_t readU8(const uint8_t *base, size_t offset)
{
    return base[offset];
}

void writeU8(uint8_t *base, size_t offset, uint8_t value)
{
    base[offset] = value;
}

const void *readPtr(const uint8_t *base, size_t offset)
{
    const void *value = nullptr;
    memcpy(&value, base + offset, sizeof(value));
    return value;
}

void writePtr(uint8_t *base, size_t offset, const void *value)
{
    memcpy(base + offset, &value, sizeof(value));
}

bool buildParam(uint8_t *param, size_t size, const Frame &frame)
{
    if (param == nullptr || size < kParamSize) return false;
    if (frame.width <= 0 || frame.height <= 0) return false;
    memset(param, 0, size);
    const uint32_t width = uint32_t(frame.width);
    const uint32_t height = uint32_t(frame.height);
    const uint32_t stride = frame.stride > 0 ? uint32_t(frame.stride) : width;
    const uint32_t slice = frame.slice > 0 ? uint32_t(frame.slice) : height;
    writeU32(param, ParamOffsets::kWidth, width);
    writeU32(param, ParamOffsets::kHeight, height);
    writeU32(param, ParamOffsets::kStride, stride);
    writeU32(param, ParamOffsets::kSlice, slice);
    writeU32(param, ParamOffsets::kPitchY, stride);
    writeU32(param, ParamOffsets::kPitchUv, stride);
    writeU32(param, ParamOffsets::kStreamType, frame.realtime ? 1 : 0);
    writeU8(param, ParamOffsets::kIsFront, frame.isFront ? 1 : 0);
    writeU32(param, ParamOffsets::kJpegRotation, uint32_t(frame.jpegRotation));
    /* The HDR per-frame arrays and the unknown face-block arrays are not fully
     * reversed yet; they stay zero (plain memset). Only the inferred rect/count
     * subset is filled. */
    if (frame.faceInfo.count > 0) {
        buildFaceInfo(param, size, frame.faceInfo, frame.width, frame.height,
                frame.isFront != 0);
    }
    if (frame.beauty.enabled) buildBeautyParams(param, size, frame.beauty);
    writeU32(param, ParamOffsets::kScesParam, uint32_t(frame.scesParam));
    if (frame.imageAdjust) {
        if (frame.sensorSensitivity <= 0) return false;
        writeU32(param, ParamOffsets::kSensorSensitivity,
                uint32_t(frame.sensorSensitivity));
        writeU8(param, ParamOffsets::kImageAdjustEnable, 1);
    }
    return true;
}

bool buildBeautyParams(uint8_t *param, size_t size, const BeautyParams &beauty)
{
    if (param == nullptr || size < kParamSize) return false;
    if (beauty.modelPath != nullptr) {
        const size_t maxLen = ParamOffsets::kBeautyModelPathSize - 1;
        const size_t length = strnlen(beauty.modelPath, maxLen);
        memcpy(param + ParamOffsets::kBeautyModelPath, beauty.modelPath, length);
    }
    const int32_t strength = beauty.strength < 0 ? 0 :
            (beauty.strength > 100 ? 100 : beauty.strength);
    writeU32(param, ParamOffsets::kBeautyStrength, uint32_t(strength));
    writeU32(param, ParamOffsets::kBeautyThreshold1, uint32_t(beauty.threshold1));
    writeU32(param, ParamOffsets::kBeautyThreshold2, uint32_t(beauty.threshold2));
    writeU32(param, ParamOffsets::kBeautyParam24, uint32_t(beauty.param24));
    writeU8(param, ParamOffsets::kBeautyMode, uint8_t(beauty.mode & 0xFF));
    writeU8(param, ParamOffsets::kBeautyEnable, uint8_t(beauty.enable & 0xFF));
    writeU32(param, ParamOffsets::kFaceFdSelect, uint32_t(beauty.fdSelect));
    writeU32(param, ParamOffsets::kFaceInfoMode, uint32_t(beauty.faceInfoMode));
    /* param+0x168 isFront is written by the geometry/flags builder. */
    return true;
}

bool buildFaceInfo(uint8_t *param, size_t size, const FaceInfo &faces,
        int32_t imageWidth, int32_t imageHeight, bool mirror)
{
    if (param == nullptr || size < kParamSize) return false;
    if (imageWidth <= 0 || imageHeight <= 0 || faces.sourceWidth <= 0 ||
            faces.sourceHeight <= 0) return false;
    const size_t block = ParamOffsets::kFaceInfoBase + ParamOffsets::kFaceBlockOffset;
    const size_t countOffset = ParamOffsets::kFaceInfoBase + ParamOffsets::kFaceCountOffset;
    /* Clear the whole face block (arrays 0..6 + count); 4..6 stay unknown zero. */
    memset(param + block, 0, ParamOffsets::kFaceInfoStride - ParamOffsets::kFaceBlockOffset);
    int32_t count = faces.count;
    if (count < 0) count = 0;
    if (count > kMaxFaceInfo) count = kMaxFaceInfo;
    int32_t written = 0;
    for (int32_t i = 0; i < count; ++i) {
        const FaceRect &face = faces.faces[i];
        if (face.width <= 0 || face.height <= 0) continue;
        int64_t left = int64_t(face.left) * imageWidth / faces.sourceWidth;
        int64_t top = int64_t(face.top) * imageHeight / faces.sourceHeight;
        int64_t width = int64_t(face.width) * imageWidth / faces.sourceWidth;
        int64_t height = int64_t(face.height) * imageHeight / faces.sourceHeight;
        if (width < 1) width = 1;
        if (height < 1) height = 1;
        if (left < 0) left = 0;
        if (top < 0) top = 0;
        if (left >= imageWidth) left = imageWidth - 1;
        if (top >= imageHeight) top = imageHeight - 1;
        if (left + width > imageWidth) width = imageWidth - left;
        if (top + height > imageHeight) height = imageHeight - top;
        if (mirror) left = imageWidth - (left + width);
        if (left < 0) left = 0;
        const size_t slot = size_t(written) * sizeof(uint32_t);
        writeU32(param, block + 0 * ParamOffsets::kFaceArrayStride + slot, uint32_t(left));
        writeU32(param, block + 1 * ParamOffsets::kFaceArrayStride + slot, uint32_t(top));
        writeU32(param, block + 2 * ParamOffsets::kFaceArrayStride + slot, uint32_t(width));
        writeU32(param, block + 3 * ParamOffsets::kFaceArrayStride + slot, uint32_t(height));
        ++written;
    }
    writeU32(param, countOffset, uint32_t(written));
    return true;
}

namespace {
/* imageCount==0 keeps the legacy single-image Frame fields; otherwise images[]
 * is authoritative. */
int32_t frameImageCount(const Frame &frame)
{
    return frame.imageCount > 0 ? frame.imageCount : 1;
}

bool frameImageAt(const Frame &frame, int32_t index, Image &image)
{
    if (frame.imageCount > 0) {
        if (index < 0 || index >= frame.imageCount ||
                index >= kMaxFrameImages) return false;
        image = frame.images[index];
        return true;
    }
    if (index != 0) return false;
    image.data = frame.data;
    image.length = frame.length;
    image.width = frame.width;
    image.height = frame.height;
    image.stride = frame.stride;
    image.slice = frame.slice;
    return true;
}
}  // namespace

bool buildFrameCtx(uint8_t *ctx, size_t size, const Frame &frame)
{
    if (ctx == nullptr || size < kFrameCtxSize) return false;
    const int32_t count = frameImageCount(frame);
    if (count <= 0 || frame.imageCount > kMaxFrameImages) return false;
    memset(ctx, 0, size);
    for (int32_t i = 0; i < count; ++i) {
        Image image;
        if (!frameImageAt(frame, i, image)) return false;
        if (image.data == nullptr || image.length == 0 ||
                image.width <= 0 || image.height <= 0) return false;
        const size_t base = size_t(i) * FrameCtxOffsets::kImageSlotStride;
        if (base + FrameCtxOffsets::kImageSlotStride > FrameCtxOffsets::kImageCount) {
            return false;
        }
        writePtr(ctx, base + FrameCtxOffsets::kImagePtr, image.data);
        writeU32(ctx, base + FrameCtxOffsets::kWidth, uint32_t(image.width));
        writeU32(ctx, base + FrameCtxOffsets::kHeight, uint32_t(image.height));
        writeU32(ctx, base + FrameCtxOffsets::kStride,
                image.stride > 0 ? uint32_t(image.stride) : uint32_t(image.width));
        writeU32(ctx, base + FrameCtxOffsets::kSlice,
                image.slice > 0 ? uint32_t(image.slice) : uint32_t(image.height));
        writeU32(ctx, base + FrameCtxOffsets::kImageFormat, kFormatYuv420Nv21);
        writeU8(ctx, base + FrameCtxOffsets::kImageValid, 1);
    }
    writeU32(ctx, FrameCtxOffsets::kImageCount, uint32_t(count));
    if (frame.scratchPtr != nullptr && frame.scratchSize >= kScratchBytes) {
        writeU32(ctx, FrameCtxOffsets::kScratchFd, uint32_t(frame.scratchFd));
        writePtr(ctx, FrameCtxOffsets::kScratchPtr, frame.scratchPtr);
        writeU32(ctx, FrameCtxOffsets::kScratchSize, uint32_t(frame.scratchSize));
    }
    return true;
}

}  // namespace oppo_platform
}  // namespace qcamera
