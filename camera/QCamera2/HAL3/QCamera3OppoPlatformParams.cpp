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
    case 5: return kAlgoFaceBeauty;       /* rear beauty */
    case 7: return kAlgoBokeh;            /* dual-camera bokeh */
    case 9: return kAlgoVdSces;           /* AI scene / SCES */
    case 10: return kAlgoArcFaceBeauty;   /* front beauty (ARC face beauty) */
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
    writeU8(param, ParamOffsets::kIsFront, frame.isFront ? 1 : 0);
    writeU32(param, ParamOffsets::kJpegRotation, uint32_t(frame.jpegRotation));
    /* The face-info record fields and the HDR per-frame arrays are located but
     * their inner semantics are not reversed yet; they stay zero (plain memset)
     * so an unknown layout cannot corrupt the algorithm input. */
    return true;
}

bool buildFrameCtx(uint8_t *ctx, size_t size, const Frame &frame)
{
    if (ctx == nullptr || size < kFrameCtxSize) return false;
    if (frame.width <= 0 || frame.height <= 0) return false;
    memset(ctx, 0, size);
    writePtr(ctx, FrameCtxOffsets::kImagePtr, frame.data);
    writeU32(ctx, FrameCtxOffsets::kWidth, uint32_t(frame.width));
    writeU32(ctx, FrameCtxOffsets::kHeight, uint32_t(frame.height));
    writeU32(ctx, FrameCtxOffsets::kStride,
            frame.stride > 0 ? uint32_t(frame.stride) : uint32_t(frame.width));
    writeU32(ctx, FrameCtxOffsets::kSlice,
            frame.slice > 0 ? uint32_t(frame.slice) : uint32_t(frame.height));
    writeU32(ctx, FrameCtxOffsets::kImageFormat, kFormatYuv420Nv21);
    writeU8(ctx, FrameCtxOffsets::kImageValid, 1);
    writeU32(ctx, FrameCtxOffsets::kImageCount, 1);
    if (frame.scratchPtr != nullptr && frame.scratchSize >= kScratchBytes) {
        writeU32(ctx, FrameCtxOffsets::kScratchFd, uint32_t(frame.scratchFd));
        writePtr(ctx, FrameCtxOffsets::kScratchPtr, frame.scratchPtr);
        writeU32(ctx, FrameCtxOffsets::kScratchSize, uint32_t(frame.scratchSize));
    }
    return true;
}

}  // namespace oppo_platform
}  // namespace qcamera
