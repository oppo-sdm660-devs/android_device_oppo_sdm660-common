/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3StockPicBest.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <errno.h>
#include <vector>

using PicBest = qcamera::QCamera3StockPicBest;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %d %s\n", __LINE__, #c); ++failures; } } while (0)

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    void *library = dlopen(argv[1], RTLD_NOW);
    if (!library) return 2;
    const auto mode = reinterpret_cast<void (*)(int)>(dlsym(library, "fake_picbest_mode"));
    const auto calls = reinterpret_cast<int (*)()>(dlsym(library, "fake_picbest_calls"));
    const auto loads = reinterpret_cast<int (*)()>(dlsym(library, "fake_picbest_loads"));
    const auto unloads = reinterpret_cast<int (*)()>(dlsym(library, "fake_picbest_unloads"));
    if (!mode || !calls || !loads || !unloads) return 2;
    PicBest::Frame frames[3] = {};
    std::vector<uint8_t> storage[3];
    for (int n = 0; n < 3; ++n) {
        storage[n].assign(80 * 52 + 96 * 26, 0xee);
        auto &frame = frames[n];
        frame.image = {storage[n].data(), storage[n].size(), 64, 48, 7, 80 * 52,
            80, 96, 52, 26};
        frame.frameNumber = 72 + n; frame.timestampNs = 1000000000 + n * 33000000;
        frame.faceCount = 2;
        frame.faces[n == 1 ? 1 : 0] = {100, 4 + n, 4, 16, 20, float(n * 5)};
        frame.faces[n == 1 ? 0 : 1] = {200, 32 + n, 4, 16, 20, 0};
        for (int row = 0; row < 48; ++row)
            memset(storage[n].data() + 7 + 80 * row, 20 + n, 64);
        for (int row = 0; row < 24; ++row)
            for (int x = 0; x < 64; x += 2) {
                storage[n][80 * 52 + 96 * row + x] = 80;
                storage[n][80 * 52 + 96 * row + x + 1] = 160;
            }
    }
    const auto original = storage[0];
    PicBest::Selection selection;
    mode(0);
    CHECK(PicBest::isAvailable(argv[1]));
    CHECK(loads() == 1 && unloads() == 1 && calls() == 0);
    mode(0);
    CHECK(PicBest::select(frames, 3, selection, nullptr, argv[1]) == 0);
    CHECK(selection.index == 1 && selection.frameNumber == 73 && selection.timestampNs == 1033000000);
    CHECK(selection.matchedFaces == 2 && selection.eyeScores[0][0] == 100);
    CHECK(selection.faceIds[0] == 100 && selection.faceIds[1] == 200);
    CHECK(loads() == 1 && unloads() == 1 && calls() == 1);
    CHECK(storage[0] == original);
    std::vector<uint8_t> fused(storage[0].size(), 0xcd);
    const auto untouched = fused;
    PicBest::Image output = frames[0].image;
    output.data = fused.data();
    mode(0);
    CHECK(PicBest::isFusionAvailable(argv[1]));
    CHECK(PicBest::fuse(frames, 3, output, selection, nullptr, argv[1]) == 0);
    CHECK(selection.fusionApplied && selection.frameNumber == 72 && selection.timestampNs == 1000000000);
    CHECK(fused[7 + 4 * 80 + 4] == 20 && fused[7 + 4 * 80 + 32] == 22);
    CHECK(fused[80 * 52 + 2 * 96 + 32] == 91 && fused[0] == 0xcd);
    CHECK(storage[0] == original && calls() == 2);
    mode(0);
    fused = untouched;
    for (auto &frame : frames)
        for (uint32_t y = 0; y < frame.image.height / 2; ++y)
            for (uint32_t x = 0; x < frame.image.width; x += 2)
                std::swap(frame.image.data[frame.image.uvOffset + y * frame.image.uvStride + x],
                          frame.image.data[frame.image.uvOffset + y * frame.image.uvStride + x + 1]);
    CHECK(PicBest::fuseNv21(frames, 3, output, selection, nullptr, argv[1]) == 0);
    CHECK(selection.fusionApplied && fused[7 + 4 * 80 + 32] == 22);
    CHECK(fused[80 * 52 + 2 * 96 + 32] == 160);
    CHECK(fused[80 * 52 + 2 * 96 + 33] == 91);
    for (auto &frame : frames)
        for (uint32_t y = 0; y < frame.image.height / 2; ++y)
            for (uint32_t x = 0; x < frame.image.width; x += 2)
                std::swap(frame.image.data[frame.image.uvOffset + y * frame.image.uvStride + x],
                          frame.image.data[frame.image.uvOffset + y * frame.image.uvStride + x + 1]);
    CHECK(storage[0] == original);
    mode(5);
    fused = untouched;
    CHECK(PicBest::fuse(frames, 3, output, selection, nullptr, argv[1]) == -7);
    CHECK(fused == untouched && storage[0] == original && !selection.fusionApplied);
    for (int errorMode : {1, 2, 4}) {
        mode(errorMode);
        selection.frameNumber = 999;
        CHECK(PicBest::select(frames, 3, selection, nullptr, argv[1]) ==
                (errorMode == 1 ? -ENOSYS : errorMode == 2 ? -EIO : -ENODATA));
        CHECK(selection.frameNumber == 0);
        CHECK(loads() == 1 && unloads() == 1);
    }
    mode(3);
    CHECK(PicBest::select(frames, 3, selection, nullptr, argv[1]) == 0);
    CHECK(selection.index == 2 && selection.frameNumber == 74);
    mode(0);
    const std::atomic<bool> cancel(true);
    CHECK(PicBest::select(frames, 3, selection, &cancel, argv[1]) == -ECANCELED);
    CHECK(loads() == 0 && calls() == 0);
    frames[1].faces[1].id = 300;
    frames[1].faces[0].id = 400;
    CHECK(PicBest::select(frames, 3, selection, nullptr, argv[1]) == -ENODATA);
    CHECK(loads() == 0);
    frames[1].faces[0].id = frames[1].faces[1].id;
    CHECK(PicBest::select(frames, 3, selection, nullptr, argv[1]) == -EINVAL);
    CHECK(PicBest::select(frames, 11, selection, nullptr, argv[1]) == -EINVAL);
    CHECK(!PicBest::isAvailable("relative.so"));
    dlclose(library);
    if (failures) return 1;
    puts("Stock PicBest eye selector tests: PASS");
    return 0;
}
