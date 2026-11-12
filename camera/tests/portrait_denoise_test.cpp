/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3StillAlgo.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <vector>

using Algo = qcamera::QCamera3StillAlgo;
static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %d %s\n", __LINE__, #c); ++failures; } } while (0)

int main() {
    constexpr unsigned width = 96, height = 64, bytes = width * height * 3 / 2;
    std::vector<uint8_t> pixels[3], truth(bytes, 128), output(bytes, 0xcd);
    Algo::Image inputs[3];
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x)
            truth[y * width + x] = 40 + (x / 12) * 16 + (y / 8 % 2) * 12;
    for (unsigned frame = 0; frame < 3; ++frame) {
        pixels[frame] = truth;
        for (unsigned i = 0; i < width * height; ++i)
            pixels[frame][i] += int((i * 17 + frame * 13) % 15) - 7;
        inputs[frame] = {pixels[frame].data(), bytes, width, height, 0, width * height,
            width, width, height, height / 2};
    }
    auto destination = inputs[0]; destination.data = output.data();
    Algo::Options options;
    Algo::Result result;
    CHECK(Algo::process(inputs, 3, destination, options, &result) == 0);
    CHECK(result.referenceIndex == 0 && result.acceptedFrames == 3 && result.pixelsChanged);
    unsigned before = 0, after = 0;
    for (unsigned i = 0; i < width * height; ++i) {
        const int a = int(pixels[0][i]) - truth[i], b = int(output[i]) - truth[i];
        before += a * a; after += b * b;
    }
    CHECK(after < before);
    CHECK(output[width * height] == 128 && pixels[0][0] == truth[0] - 7);
    CHECK(Algo::process(inputs, 3, inputs[0], options, &result) == 0);
    CHECK(result.pixelsChanged && pixels[0] == output);
    for (auto &source : pixels) source = truth;
    CHECK(Algo::process(inputs, 3, destination, options, &result) == 0);
    CHECK(!result.pixelsChanged && output == truth);
    output.assign(bytes, 0xcd);
    const std::atomic<bool> cancelled(true);
    CHECK(Algo::process(inputs, 3, destination, options, &result, &cancelled) == -ECANCELED);
    CHECK(output == std::vector<uint8_t>(bytes, 0xcd));
    CHECK(Algo::process(inputs, 0, destination, options, &result) == -EINVAL);
    CHECK(output == std::vector<uint8_t>(bytes, 0xcd));
    if (failures) return 1;
    puts("Portrait denoise numeric/alias/cancel tests: PASS");
}
