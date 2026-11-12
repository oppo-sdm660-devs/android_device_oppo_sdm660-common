/* SPDX-License-Identifier: BSD-3-Clause */
#include <atomic>
#include <cstdint>
#include <cstring>
#include <new>

struct Image {
    uint32_t format;
    int32_t width, height;
    uint8_t *planes[4];
    int32_t pitches[4];
};
struct Calibration { uint8_t *data; int32_t length; };
struct Geometry { int32_t wideWidth, wideHeight, teleWidth, teleHeight; };
struct Param {
    int32_t x, y, blur, mode;
    void *rectangles, *orientations;
    int32_t faces, reserved;
};
struct Engine { int frames = 0; bool calibration = false, geometry = false, degree = false; };
static int mode = 0, inits = 0, uninits = 0, processes = 0;
static std::atomic<bool> *cancel = nullptr;
extern "C" {
void fake_preview_mode(int value) { mode = value; }
void fake_preview_cancel(std::atomic<bool> *flag) { cancel = flag; }
int fake_preview_inits() { return inits; }
int fake_preview_uninits() { return uninits; }
int fake_preview_processes() { return processes; }
int ARC_DCVR_Init(void **handle) {
    ++inits;
    if (mode == 1) return -5;
    if (mode == 2) { *handle = nullptr; return 0; }
    *handle = new (std::nothrow) Engine;
    return *handle ? 0 : -12;
}
int ARC_DCVR_Uninit(void **handle) { ++uninits; delete static_cast<Engine *>(*handle); *handle = nullptr; return 0; }
int ARC_DCVR_SetCaliData(void *handle, Calibration *calibration) {
    if (mode == 3) return -5;
    auto &engine = *static_cast<Engine *>(handle);
    engine.calibration = calibration->length == 512 && calibration->data;
    return engine.calibration ? 0 : -22;
}
int ARC_DCVR_SetCameraImageInfo(void *handle, Geometry *geometry) {
    auto &engine = *static_cast<Engine *>(handle);
    engine.geometry = engine.calibration && geometry->wideWidth >= 64 &&
            geometry->wideHeight >= 48 && geometry->teleWidth >= 32 && geometry->teleHeight >= 24;
    return engine.geometry ? 0 : -22;
}
int ARC_DCVR_SetImageDegree(void *handle, int value) {
    auto &engine = *static_cast<Engine *>(handle);
    engine.degree = engine.geometry && value == 270;
    return engine.degree ? 0 : -22;
}
int ARC_DCVR_GetDefaultParam(Param *param) {
    param->x = param->y = -1; param->blur = 100; param->mode = 1;
    return 0;
}
int ARC_DCVR_Process(void *handle, Image *wide, Image *tele, Image *destination,
        Param *param, int32_t *effect) {
    auto &engine = *static_cast<Engine *>(handle);
    ++processes;
    if (!engine.degree || wide->format != 0x802 || tele->format != 0x802 ||
            destination->format != 0x802 || param->mode != 1 || param->rectangles ||
            param->orientations || param->faces || param->reserved || param->blur != 70 ||
            param->x != wide->width / 2 || param->y != wide->height / 2 ||
            wide->planes[1][0] != 91 || tele->planes[1][0] != 93) return -22;
    *effect = 42;
    for (int plane = 0; plane < 2; ++plane)
        for (int row = 0; row < (plane ? wide->height / 2 : wide->height); ++row)
            std::memcpy(destination->planes[plane] + row * destination->pitches[plane],
                    wide->planes[plane] + row * wide->pitches[plane], wide->width);
    if (++engine.frames > 5 && mode != 4) destination->planes[0][0] ^= 0x40;
    wide->planes[0][5] = 0; tele->planes[0][5] = 0; // Adapters must isolate both inputs.
    if (cancel) cancel->store(true);
    if (mode == 5) return -5;
    if (mode == 6) destination->planes[0]++;
    return 0;
}
}
