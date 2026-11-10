/* SPDX-License-Identifier: BSD-3-Clause */
// Synthetic-input device smoke; pixel differences are not a stock quality verdict.
#include "QCamera3StockSuperPhoto.h"
#include <algorithm>
#include <cmath>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
using Algo = qcamera::QCamera3StockSuperPhoto;
namespace {
uint8_t byte(double value) { return uint8_t(std::clamp(value, 0.0, 255.0) + .5); }
double scene(double x, double y, int width, int height) {
    constexpr double pi = 3.141592653589793;
    const double low = 27 * sin(2 * pi * x / 23) + 23 * cos(2 * pi * y / 29) +
            12 * sin(2 * pi * (x + 1.7 * y) / 41);
    const double fine = 18 * sin(2 * pi * x * .62) * cos(2 * pi * y * .23) +
            14 * cos(2 * pi * y * .61) * sin(2 * pi * x * .17);
    const double shape = x > width * .43 && x < width * .57 &&
            y > height * .42 && y < height * .57 ? 18 : -6;
    return 128 + low + fine + shape;
}
struct Frame {
    std::vector<uint8_t> bytes;
    Algo::Image image;
    Frame(unsigned width, unsigned height) : bytes(size_t(width) * height * 3 / 2, 128),
        image{bytes.data(), bytes.size(), width, height, 0, size_t(width) * height,
            width, width, height, height / 2} {}
};
double sample(const Frame &frame, double x, double y) {
    const unsigned ix = unsigned(x), iy = unsigned(y);
    const double fx = x - ix, fy = y - iy;
    const auto at = [&](unsigned xx, unsigned yy) {
        return frame.bytes[size_t(yy) * frame.image.width + xx];
    };
    return at(ix, iy) * (1 - fx) * (1 - fy) + at(ix + 1, iy) * fx * (1 - fy) +
            at(ix, iy + 1) * (1 - fx) * fy + at(ix + 1, iy + 1) * fx * fy;
}
double mae(const Frame &a, const Frame &b) {
    double error = 0;
    const unsigned width = a.image.width, height = a.image.height;
    for (unsigned y = 24; y + 24 < height; ++y)
        for (unsigned x = 24; x + 24 < width; ++x)
            error += std::abs(int(a.bytes[size_t(y) * width + x]) - int(b.bytes[size_t(y) * width + x]));
    return error / ((width - 48) * (height - 48));
}
double rmse(const Frame &a, const Frame &b) {
    double error = 0;
    const unsigned width = a.image.width, height = a.image.height;
    for (unsigned y = 24; y + 24 < height; ++y)
        for (unsigned x = 24; x + 24 < width; ++x) {
            const double d = int(a.bytes[size_t(y) * width + x]) - int(b.bytes[size_t(y) * width + x]);
            error += d * d;
        }
    return sqrt(error / ((width - 48) * (height - 48)));
}
bool save(const std::string &directory, const char *name, const Frame &frame) {
    FILE *file = fopen((directory + "/" + name + ".nv21").c_str(), "wb");
    if (!file) return false;
    const bool ok = fwrite(frame.bytes.data(), 1, frame.bytes.size(), file) == frame.bytes.size();
    fclose(file);
    return ok;
}
}
int main(int argc, char **argv) {
    if (argc < 2) return 1;
    const unsigned width = argc > 2 ? unsigned(atoi(argv[2])) : 640;
    const unsigned height = argc > 3 ? unsigned(atoi(argv[3])) : 480;
    if (width < 64 || height < 64 || width > 4608 || height > 3456 || (width & 3) || (height & 3)) return 1;
    for (const char *library : {"libVDSuperPhotoAPI.so", "libcamera_core_hwi.so"}) {
        void *handle = dlopen(library, RTLD_NOW | RTLD_LOCAL);
        printf("dlopen %s: %s\n", library, handle ? "OK" : dlerror());
        if (handle) dlclose(handle);
    }
    printf("2x_available=%d width=%u height=%u\n", Algo::is2xAvailable(), width, height);
    fflush(stdout);
    Frame frames[4] = {Frame(width, height), Frame(width, height), Frame(width, height), Frame(width, height)};
    Algo::Image images[4], identical[4];
    Algo::Source sources[4];
    const double shifts[4][2] = {{0, 0}, {.5, 0}, {0, .5}, {.5, .5}};
    for (unsigned i = 0; i < 4; ++i) {
        for (unsigned y = 0; y < height; ++y)
            for (unsigned x = 0; x < width; ++x)
                frames[i].bytes[size_t(y) * width + x] = byte(scene(x + shifts[i][0], y + shifts[i][1], width, height));
        images[i] = frames[i].image;
        identical[i] = frames[0].image;
        sources[i] = {100 + i, 1000000000 + int64_t(i) * 33333333, 10000000, 200};
    }
    Frame truth(width, height), interpolation(width, height), shifted(width, height), repeated(width, height);
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x) {
            const double sx = width / 4.0 + x / 2.0, sy = height / 4.0 + y / 2.0;
            truth.bytes[size_t(y) * width + x] = byte(scene(sx, sy, width, height));
            interpolation.bytes[size_t(y) * width + x] = byte(sample(frames[0], sx, sy));
        }
    Algo::ZoomOptions options;
    options.fullFieldOfView = true;
    Algo::ZoomResult result;
    const int32_t rc = Algo::process2x(images, sources, 4, shifted.image, "s5k2p7sq", options, &result);
    printf("shifted_status=%d applied=%d submitted=%u\n", rc, result.algorithmApplied, result.submittedFrames);
    fflush(stdout);
    if (rc) return 2;
    const int32_t same = Algo::process2x(identical, sources, 4, repeated.image, "s5k2p7sq", options, nullptr);
    printf("identical_status=%d\n", same);
    if (same) return 3;
    printf("{\"width\":%u,\"height\":%u,\"mae_vs_bilinear\":%.6f,\"mae_vs_identical_burst\":%.6f,"
           "\"rmse_truth_bilinear\":%.6f,\"rmse_truth_shifted\":%.6f,\"rmse_truth_identical\":%.6f}\n",
            width, height, mae(shifted, interpolation), mae(shifted, repeated), rmse(truth, interpolation),
            rmse(truth, shifted), rmse(truth, repeated));
    bool saved = save(argv[1], "shifted", shifted) && save(argv[1], "identical", repeated) &&
            save(argv[1], "bilinear", interpolation) && save(argv[1], "truth", truth);
    for (unsigned i = 0; i < 4; ++i) saved &= save(argv[1], ("input" + std::to_string(i)).c_str(), frames[i]);
    if (!saved) return 4;
    return mae(shifted, interpolation) > .01 && mae(shifted, repeated) > .01 ? 0 : 5;
}
