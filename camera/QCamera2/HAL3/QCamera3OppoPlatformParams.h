/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_OPPO_PLATFORM_PARAMS_H
#define QCAMERA3_OPPO_PLATFORM_PARAMS_H

#include <stddef.h>
#include <stdint.h>

namespace qcamera {

/*
 * Reversed OPPO platform algorithm block used by the original HAL1 path
 * (libcamera_core_hwi.so in-process wrapper / liboppo_platform_hwi.so client).
 *
 * The algorithm mask values below are the entries of the core wrapper registry
 * (libcamera_core_hwi.so .data.rel.ro, 17 entries x 0x18, first word = bit).
 * The param/frame-ctx offsets come from the HWI wrappers
 * (e.g. liboppo_platform_hwi.so hdr_process_hwi / llhdr_process_hwi /
 * vdscesprocess_process_hwi) and from libcamera_core_hwi.so *_process bodies.
 */
namespace oppo_platform {

enum AlgoBit : uint64_t {
    kAlgoBlurless = 1ULL << 0,
    kAlgoSuperPhoto = 1ULL << 1,
    kAlgoStFaceBlur = 1ULL << 2,
    kAlgoFilter = 1ULL << 3,
    kAlgoFaceInfo = 1ULL << 4,
    kAlgoFaceBeauty = 1ULL << 5,
    kAlgoBokeh = 1ULL << 6,
    kAlgoHdr = 1ULL << 7,
    kAlgoArcFaceBeauty = 1ULL << 8,
    kAlgoVdSces = 1ULL << 9,
    kAlgoAsd = 1ULL << 10,
    kAlgoPicBest = 1ULL << 11,
    kAlgoImageAdjust = 1ULL << 12,
    kAlgoPortraitLight = 1ULL << 13,
    kAlgoLlhdr = 1ULL << 14,
    kAlgoSlam = 1ULL << 15,
    kAlgoOffline = 1ULL << 62,
};

/* bit63 selects the in-process backend (libcamera_core_hwi oppo_wrapper_*). */
constexpr uint64_t kInProcessBackend = 1ULL << 63;

constexpr size_t kParamSize = 0x12EC;
constexpr size_t kFrameCtxSize = 0x308;

struct ParamOffsets {
    static constexpr size_t kWidth = 0x000;
    static constexpr size_t kHeight = 0x004;
    static constexpr size_t kStride = 0x008;
    static constexpr size_t kSlice = 0x00c;
    static constexpr size_t kFlipMode = 0x054;
    static constexpr size_t kVisiontechFilterType = 0x160;
    static constexpr size_t kIsFront = 0x168;          /* u8 */
    static constexpr size_t kOppoCapModeB = 0x169;     /* u8 */
    static constexpr size_t kVendorCustomApi = 0x171;  /* u8 */
    static constexpr size_t kPortraitLightMode = 0x10C0;
    static constexpr size_t kJpegRotation = 0x10C4;
    static constexpr size_t kOarNeedStop = 0x10E0;
    /* 10-slot face-info records (bokeh_setup_parameters_hwi memcpy4-copies each
     * 0x174-byte entry). Inner field layout is not reversed yet, so the block is
     * only zeroed and face_beauty/bokeh stay on their default branch. */
    static constexpr size_t kFaceInfoBase = 0x0214;
    static constexpr size_t kFaceInfoStride = 0x174;
    static constexpr int kFaceInfoCount = 10;
    /* Face block inside one 0x174 image descriptor: +0x28..+0x174.
     * Arrays 0..3 are the inferred left/top/width/height int32[10]; arrays 4..6
     * and all other fields are not reversed yet and stay 0. Count at +0x170. */
    static constexpr size_t kFaceBlockOffset = 0x28;
    static constexpr size_t kFaceArrayStride = 0x28;
    static constexpr size_t kFaceCountOffset = 0x170;
    /* Original beauty/SCES parameters (RE task-74). */
    static constexpr size_t kBeautyModelPath = 0x0060;   /* char[0x80] */
    static constexpr size_t kBeautyModelPathSize = 0x80;
    static constexpr size_t kBeautyParam24 = 0x0024;
    static constexpr size_t kBeautyThreshold1 = 0x0158;
    static constexpr size_t kBeautyThreshold2 = 0x015C;
    static constexpr size_t kBeautyStrength = 0x0164;
    static constexpr size_t kBeautyMode = 0x016A;        /* u8 */
    static constexpr size_t kBeautyEnable = 0x016F;      /* u8 */
    static constexpr size_t kFaceFdSelect = 0x01F0;
    static constexpr size_t kFaceInfoMode = 0x01F4;
    static constexpr size_t kScesParam = 0x10BC;
    /* 10-frame per-frame parameter arrays read by hdr/llhdr (arrays of 10 u32). */
    static constexpr size_t kFrameArray0 = 0x023C;
    static constexpr size_t kFrameArray1 = 0x0264;
    static constexpr size_t kFrameArray2 = 0x028C;
    static constexpr size_t kFrameArray3 = 0x02B4;
    static constexpr size_t kFrameFloat = 0x02DC;
    static constexpr size_t kFrameArray4 = 0x0354;
    static constexpr size_t kFrameInfo = 0x0384;       /* u32, hdr/llhdr/... read */
};

struct FrameCtxOffsets {
    /* p_buffer_construct_t: image slots are 0x40 bytes each (RE task-62/67:
     * setup_ocam_input_buffers stride 0x40, valid/primary byte at +0x38, count
     * at +0x280). Slot 0 is also the legacy single-frame set. */
    static constexpr size_t kImageSlotStride = 0x40;
    static constexpr size_t kImagePtr = 0x00;      /* void*: NV21 buffer */
    static constexpr size_t kWidth = 0x10;
    static constexpr size_t kHeight = 0x14;
    static constexpr size_t kStride = 0x18;
    static constexpr size_t kSlice = 0x1C;
    static constexpr size_t kImageFormat = 0x30;   /* u32 cam_format_t */
    static constexpr size_t kImageValid = 0x38;    /* u8 valid/primary */
    static constexpr size_t kImageCount = 0x280;   /* u32 image count */
    /* Scratch descriptor; core facebeauty/imageadjust/superphoto/blurless read
     * +0x2cc without a null check. */
    static constexpr size_t kScratchFd = 0x2C8;    /* u32 */
    static constexpr size_t kScratchPtr = 0x2CC;   /* void* */
    static constexpr size_t kScratchSize = 0x2D4;  /* u32 */
};

/* cam_format_t CAM_FORMAT_YUV_420_NV21 (cam_types.h): HAL3 hands NV21 frames. */
constexpr uint32_t kFormatYuv420Nv21 = 2;
/* HAL1 allocates 0x1000 bytes for the HWI scratch (0x400 for bokeh). */
constexpr size_t kScratchBytes = 0x1000;

constexpr int32_t kMaxFaceInfo = 10;

/* One HAL3-detected face rect in the coordinate space given by FaceInfo. */
struct FaceRect {
    int32_t left = 0;
    int32_t top = 0;
    int32_t width = 0;
    int32_t height = 0;
};

/* Face set handed to the original face-beauty/SCES blocks. sourceWidth/Height is
 * the coordinate space of faces[] (HAL3 active array); buildFaceInfo scales it to
 * the image geometry and mirrors it for the front camera. */
struct FaceInfo {
    int32_t count = 0;
    int32_t sourceWidth = 0;
    int32_t sourceHeight = 0;
    FaceRect faces[kMaxFaceInfo];
};

/* Original beauty parameters (task-74); only evidence-backed fields are filled. */
struct BeautyParams {
    bool enabled = false;
    const char *modelPath = nullptr; /* param+0x60 (Venus resource dir/file) */
    int32_t strength = 0;            /* 0..100 -> param+0x164 */
    int32_t param24 = 0;             /* param+0x24 (D) */
    int32_t threshold1 = 0;          /* param+0x158 (D, keep 0) */
    int32_t threshold2 = 0;          /* param+0x15C (D, keep 0) */
    int32_t mode = 0;                /* param+0x16A u8 (D switch) */
    int32_t enable = 0;              /* param+0x16F u8 (D switch) */
    int32_t fdSelect = 0;            /* param+0x1F0 (D, Venus FD) */
    int32_t faceInfoMode = 0;        /* param+0x1F4 (D, Venus FD) */
};

constexpr int32_t kMaxFrameImages = 10;

/* One NV21 input image (contiguous Y plane followed by UV is required by the
 * original frame ctx; HAL3 bracket copies are contiguous). */
struct Image {
    const uint8_t *data = nullptr;
    size_t length = 0;
    int32_t width = 0;
    int32_t height = 0;
    int32_t stride = 0;
    int32_t slice = 0;
};

/* One NV21 snapshot frame (multi-image engine inputs), as handed to the platform
 * algorithm block. imageCount==0 keeps the legacy single-image fields. */
struct Frame {
    const uint8_t *data = nullptr;
    size_t length = 0;
    int32_t width = 0;
    int32_t height = 0;
    int32_t stride = 0;
    int32_t slice = 0;
    int32_t isFront = 0;
    int32_t jpegRotation = 0;
    /* Session-level scratch descriptor; -1/null/0 leaves ctx+0x2c8/2cc/2d4 zero. */
    int32_t scratchFd = -1;
    void *scratchPtr = nullptr;
    size_t scratchSize = 0;
    FaceInfo faceInfo;
    BeautyParams beauty;
    int32_t scesParam = 0;               /* param+0x10BC (SCES) */
    /* P3 multi-frame inputs: current-capture bracket only (never ZSL history).
     * imageCount==0 falls back to the legacy single-image fields above. */
    int32_t imageCount = 0;
    Image images[kMaxFrameImages];
};

/* Explicit org.lineageos stillMode -> original algorithm bit. A zero result means
 * the mode has no reversed single-frame platform algorithm. */
uint64_t maskForStillMode(int32_t stillMode);

/* True when every requested bit is one of the reversed single-frame algorithms. */
bool supportedMask(uint64_t mask);

/* Modes the HAL3 platform path accepts (RE evidence):
 * SCES/AI scene (bit9), front beauty (bit8), rear beauty (bit5), plus SUPER(bit1)
 * as a property-gated single-frame probe. HDR/LLHDR/MFNR/bokeh stay excluded:
 * the original platform bits are unreachable or need unproven multi-frame/bokeh
 * input contracts (see OCam/docs/hal1-engine-wiring.md). */
constexpr uint64_t kSupportedPlatformMask =
        kAlgoVdSces | kAlgoArcFaceBeauty | kAlgoFaceBeauty | kAlgoFaceInfo |
        kAlgoSuperPhoto;
bool isPlatformStillMode(int32_t stillMode);

/* Stable name for a single algorithm bit, for diagnostics. */
const char *algorithmName(uint64_t bit);

/* Zero-fill and fill the geometry/flags of one param_platform_t block. */
bool buildParam(uint8_t *param, size_t size, const Frame &frame);

/* Fill the platform param face block of image descriptor 0 (single-image path):
 * count + rect arrays (left/top/width/height), scaled from faces.sourceWidth/Height
 * to imageWidth/Height and mirrored for the front camera. Unknown arrays stay 0.
 * Returns false on bad size/geometry; count=0 clears the block. */
bool buildFaceInfo(uint8_t *param, size_t size, const FaceInfo &faces,
        int32_t imageWidth, int32_t imageHeight, bool mirror);

/* Fill the original beauty parameters (param+0x60/0x164/0x168/0x24/0x158/0x15C/
 * 0x16A/0x16F/0x1F0/0x1F4). The caller must only enable this when the model path
 * exists; unknown/D fields stay whatever the caller passes (0 recommended). */
bool buildBeautyParams(uint8_t *param, size_t size, const BeautyParams &beauty);

/* Zero-fill and fill image slot 0/scratch of one platform frame context block. */
bool buildFrameCtx(uint8_t *ctx, size_t size, const Frame &frame);

uint32_t readU32(const uint8_t *base, size_t offset);
void writeU32(uint8_t *base, size_t offset, uint32_t value);
uint8_t readU8(const uint8_t *base, size_t offset);
void writeU8(uint8_t *base, size_t offset, uint8_t value);
const void *readPtr(const uint8_t *base, size_t offset);
void writePtr(uint8_t *base, size_t offset, const void *value);

}  // namespace oppo_platform
}  // namespace qcamera

#endif /* QCAMERA3_OPPO_PLATFORM_PARAMS_H */
