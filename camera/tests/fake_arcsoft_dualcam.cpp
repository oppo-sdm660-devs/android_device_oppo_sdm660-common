/* SPDX-License-Identifier: BSD-3-Clause */
#include <cstdint>
#include <cstring>

struct ArcImage {
    uint32_t format;
    int32_t width, height;
    uint8_t *planes[4];
    int32_t pitches[4];
};
struct Calibration { uint8_t *data; int32_t length; };
struct CameraImageInfo { int32_t wideWidth, wideHeight, teleWidth, teleHeight; };
struct DisparityParam {
    int32_t degree;
    float maxFov;
    void *faceRects, *faceOrientations;
    int32_t faceCount;
};
struct RefocusParam { int32_t x, y, blurLevel, useRect, left, top, right, bottom; };
static int mode = 0, initCount = 0, uninitCount = 0, calculateCount = 0, processCount = 0;
static int engine;
extern "C" {
void fake_dual_mode(int value) {
    mode = value;
    initCount = uninitCount = calculateCount = processCount = 0;
}
int fake_dual_inits() { return initCount; }
int fake_dual_uninits() { return uninitCount; }
int fake_dual_calculates() { return calculateCount; }
int fake_dual_processes() { return processCount; }
int ARC_DCIR_Init(void **handle, int32_t value) {
    ++initCount;
    if (value != 1) return -22;
    *handle = &engine;
    return 0;
}
int ARC_DCIR_Uninit(void **handle) {
    ++uninitCount;
    if (*handle != &engine) return -22;
    *handle = nullptr;
    return 0;
}
int ARC_DCIR_SetCaliData(void *, Calibration *value) {
    return value->data && value->length == 512 ? 0 : -22;
}
int ARC_DCIR_Reset(void *) { return 0; }
int ARC_DCIR_SetCameraImageInfo(void *, CameraImageInfo *info) {
    return info->wideWidth == 64 && info->wideHeight == 48 &&
            info->teleWidth == 64 && info->teleHeight == 48 ? 0 : -22;
}
int ARC_DCIR_CalcDisparityData(void *, ArcImage *wide, ArcImage *tele, DisparityParam *param) {
    ++calculateCount;
    if (wide->format != 0x802 || tele->format != 0x802 || wide->width != 64 ||
            tele->height != 48 || wide->planes[0][10] != 10 ||
            tele->planes[0][10] != 200 || param->degree != 270 || param->maxFov != 62 ||
            param->faceCount != 0) return -22;
    return mode == 1 ? -5 : 0;
}
int ARC_DCIR_GetDisparityDataSize(void *, int32_t *size) { *size = 16; return 0; }
int ARC_DCIR_GetDisparityData(void *, uint8_t *data) { std::memset(data, 1, 16); return 0; }
int ARC_DCIR_Process(void *, uint8_t *, int32_t size, ArcImage *source,
        RefocusParam *param, ArcImage *destination) {
    ++processCount;
    if (size != 16 || param->x != 32 || param->y != 24 || param->blurLevel != 20) return -22;
    for (int row = 0; row < source->height; ++row)
        std::memcpy(destination->planes[0] + row * destination->pitches[0],
                source->planes[0] + row * source->pitches[0], source->width);
    for (int row = 0; row < source->height / 2; ++row)
        std::memcpy(destination->planes[1] + row * destination->pitches[1],
                source->planes[1] + row * source->pitches[1], source->width);
    return 0;
}
}
