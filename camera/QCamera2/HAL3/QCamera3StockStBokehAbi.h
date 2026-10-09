/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_STOCK_ST_BOKEH_ABI_H
#define QCAMERA3_STOCK_ST_BOKEH_ABI_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <dlfcn.h>

namespace qcamera {
namespace stockst {

// R11/R11s libSTDualCamPreviewBokeh.so, SHA256 f91161987782ff37… .
struct Stream {
    int32_t width, height, stride, scanline;
};

struct Calibration {
    const uint8_t *data;
    int32_t length;
};

struct CameraGeometry {
    int32_t calibrationWidth, calibrationHeight;
    int32_t centeredWindowWidth, centeredWindowHeight;
    int32_t unusedBySt[6];
};

struct CameraImageInfo {
    CameraGeometry main, auxiliary;
};

struct Metadata {
    int32_t faceCount;
    int32_t selectedFace[4];
    uint8_t faceAndReserved[0xb8];
    uint32_t mainMetadataWord;
    float exposure, gain;
    int32_t afStateFlag, afDoneFlag;
    uint32_t focusRoiType;
    float lensShiftUm;
    int32_t farField, unusedEc, nearField, focusDistance;
};

struct Image {
    Metadata metadata;
    int32_t format;
    int32_t width, height, stride, scanline;
    uint8_t *y, *chromaSecondByte, *chroma;
};

struct Refocus {
    int32_t x, y, width, height;
    int32_t unusedBySt;
    int32_t mode;
    int32_t blurLevel;
};

struct Api {
    int32_t (*initialize)(void **, const Stream *, const Stream *, const Stream *) = nullptr;
    int32_t (*calibration)(void *, const Calibration *) = nullptr;
    int32_t (*geometry)(void *, const CameraImageInfo *) = nullptr;
    int32_t (*rotation)(void *, int32_t) = nullptr;
    int32_t (*inputs)(void *, const Image *, const Image *) = nullptr;
    int32_t (*process)(void *, const Image *, Image *, const void *,
            const Refocus *, int32_t *) = nullptr;
    int32_t (*reset)(void *) = nullptr;
    int32_t (*uninitialize)(void **) = nullptr;

    bool load(void *library) {
        if (!library) return false;
#define ST_LOAD(member, symbol) \
        member = reinterpret_cast<decltype(member)>(dlsym(library, symbol))
        ST_LOAD(initialize, "rtbInitialize");
        ST_LOAD(calibration, "rtbSetCalibData");
        ST_LOAD(geometry, "rtbSetCameraImageInfo");
        ST_LOAD(rotation, "rtbSetInputRotation");
        ST_LOAD(inputs, "rtbCalcDepthMap");
        ST_LOAD(process, "rtbdoBokehEffect");
        ST_LOAD(reset, "rtbReset");
        ST_LOAD(uninitialize, "rtbdeInitialize");
#undef ST_LOAD
        return initialize && calibration && geometry && rotation && inputs && process &&
                reset && uninitialize;
    }
};

inline CameraGeometry centeredGeometry(int32_t calibrationWidth, int32_t calibrationHeight,
        int32_t windowWidth, int32_t windowHeight) {
    return {calibrationWidth, calibrationHeight, windowWidth, windowHeight, {}};
}

inline void setNv21Layout(Image &image, const Stream &stream, uint8_t *y, uint8_t *vu) {
    // imageFormatType_t from Oppo MapToAlgoFormat; this is not ASVL's 0x802.
    image.format = 0;
    image.width = stream.width;
    image.height = stream.height;
    image.stride = stream.stride;
    image.scanline = stream.scanline;
    image.y = y;
    image.chromaSecondByte = vu + 1;
    image.chroma = vu;
}

static_assert(sizeof(Stream) == 0x10, "ST stream layout");
static_assert(sizeof(CameraGeometry) == 0x28, "ST per-camera geometry layout");
static_assert(sizeof(CameraImageInfo) == 0x50, "ST camera image info layout");
static_assert(sizeof(Refocus) == 0x1c, "ST consumed refocus span");
static_assert(sizeof(Metadata) == 0xf8, "ST input metadata span");
static_assert(offsetof(Metadata, selectedFace) == 4, "ST selected face xywh offset");
static_assert(offsetof(Metadata, exposure) == 0xd0, "ST exposure offset");
static_assert(offsetof(Metadata, lensShiftUm) == 0xe4, "ST lens shift offset");
static_assert(offsetof(Image, format) == 0xf8, "ST image format offset");
static_assert(offsetof(Image, width) == 0xfc, "ST image width offset");
static_assert(offsetof(Refocus, mode) == 0x14, "ST refocus default mode offset");
static_assert(offsetof(Refocus, blurLevel) == 0x18, "ST blur level offset");
#if UINTPTR_MAX == UINT32_MAX
static_assert(sizeof(Calibration) == 8, "ST ARM32 calibration layout");
static_assert(sizeof(Image) == 0x118, "ST ARM32 image layout");
static_assert(offsetof(Image, y) == 0x10c, "ST ARM32 Y offset");
static_assert(offsetof(Image, chroma) == 0x114, "ST ARM32 VU offset");
#endif

} // namespace stockst
} // namespace qcamera

#endif
