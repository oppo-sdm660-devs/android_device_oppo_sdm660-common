/* SPDX-License-Identifier: BSD-3-Clause */
#include <stdint.h>
#include <cstring>

struct Libraries { void *slots[18]; };
struct FaceBlock {
    int32_t left[10], top[10], width[10], height[10], attribute[10];
    float roll[10];
    int32_t id[10], reserved[10], sourceWidth, sourceHeight, count;
};
static int mode = 0, loads = 0, unloads = 0, calls = 0;
extern "C" void fake_picbest_mode(int value) { mode = value; loads = unloads = calls = 0; }
extern "C" int fake_picbest_calls() { return calls; }
extern "C" int fake_picbest_loads() { return loads; }
extern "C" int fake_picbest_unloads() { return unloads; }
int load(Libraries *) asm("_Z15picbest_loadlibP20picbest_static_lib_t");
int unload(Libraries *) asm("_Z17picbest_unloadlibP20picbest_static_lib_t");
int eyes(Libraries *, FaceBlock *, int, int, int, int, uint8_t **, uint8_t **,
        int (*)[10], int (*)[4], int) asm("_Z21executeEyesOpenDetectP20picbest_static_lib_tP20platform_face_data_tiiiiPPhS4_PA10_iPA4_ii");
int fuse(Libraries *, int, int, int, int, uint8_t **, uint8_t **, int, FaceBlock *,
        int (*)[10]) asm("_Z14picBestProcessP20picbest_static_lib_tiiiiPPhS2_iP20platform_face_data_tPA10_i");
int load(Libraries *libraries) {
    ++loads;
    for (auto &slot : libraries->slots) slot = reinterpret_cast<void *>(uintptr_t(1));
    return mode == 1 ? -1 : 0;
}
int unload(Libraries *libraries) {
    ++unloads;
    std::memset(libraries, 0, sizeof(*libraries));
    return 0;
}
int eyes(Libraries *, FaceBlock *blocks, int width, int height, int stride, int slice,
        uint8_t **y, uint8_t **uv, int (*scores)[10], int (*regions)[4], int count) {
    ++calls;
    if (width != 64 || height != 48 || stride != width || slice != height || count != 3) return 0;
    for (int n = 0; n < count; ++n) {
        const auto &face = blocks[n];
        if (face.sourceWidth != width || face.sourceHeight != height || face.count != 2 ||
                face.id[0] != 100 || face.id[1] != 200 || face.left[0] != 4 + n ||
                face.left[1] != 32 + n || face.width[0] != 16 || face.height[0] != 20 ||
                face.roll[0] != float(n * 5) || y[n][0] != uint8_t(20 + n) ||
                uv[n][0] != 80 || uv[n][1] != 160) return 0;
        regions[n][0] = face.left[0];
        scores[n][0] = mode == 3 ? 10 : (n == 0 ? 100 : n == 1 ? 70 : 60);
        scores[n][1] = mode == 3 ? 10 : (n == 0 ? 20 : n == 1 ? 80 : 90);
    }
    if (mode == 4) for (int n = 0; n < count; ++n) scores[n][1] = -1;
    return mode == 2 ? 0 : 1;
}
int fuse(Libraries *, int width, int height, int stride, int slice, uint8_t **y,
        uint8_t **uv, int count, FaceBlock *blocks, int (*scores)[10]) {
    ++calls;
    if (width != 64 || height != 48 || stride != width || slice != height ||
            count != 3 || blocks[0].count != 2 || scores[0][0] != 100 || scores[2][1] != 90)
        return -1;
    // Both subjects come from different source frames; the reference background stays.
    y[0][4 * width + 32] = y[2][4 * width + 34];
    uv[0][2 * width + 32] = 91;
    return mode == 5 ? -7 : 0;
}
