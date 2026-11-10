/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3HdrAlgo.h"
#include "QCamera3LowLightHdrAlgo.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <errno.h>
#include <string>
#include <vector>

using qcamera::QCamera3HdrAlgo;
using qcamera::QCamera3LowLightHdrAlgo;

namespace {
struct Frame {
    std::vector<uint8_t> pixels;
    QCamera3HdrAlgo::Image image;
    Frame(uint32_t width, uint32_t height) : pixels(size_t(width) * height * 3 / 2),
            image{pixels.data(), pixels.size(), width, height, 0,
                size_t(width) * height, width, width, height, height / 2} {}
};

uint64_t hash(const std::vector<uint8_t> &pixels) {
    uint64_t result = UINT64_C(14695981039346656037);
    for (uint8_t value : pixels) result = (result ^ value) * UINT64_C(1099511628211);
    return result;
}

void fill(Frame &frame, float gain, unsigned noiseSeed) {
    const unsigned width = frame.image.width, height = frame.image.height;
    for (unsigned y = 0; y < height; ++y) {
        for (unsigned x = 0; x < width; ++x) {
            const float ramp = 24.f + 174.f * x / (width - 1);
            const float texture = 14.f * std::sin(x * 0.23f) * std::cos(y * 0.19f) +
                    (((x / 19 + y / 17) & 1) ? 14.f : -14.f);
            const int noise = noiseSeed ? int((x * 17 + y * 31 + noiseSeed * 13) % 7) - 3 : 0;
            const int value = int((ramp + texture) * gain) + noise;
            frame.pixels[size_t(y) * width + x] = uint8_t(value < 1 ? 1 : value > 254 ? 254 : value);
        }
    }
    for (unsigned y = 0; y < height / 2; ++y) {
        for (unsigned x = 0; x < width; x += 2) {
            const size_t offset = frame.image.uvOffset + size_t(y) * width + x;
            frame.pixels[offset] = uint8_t(116 + (x / 32) % 24);
            frame.pixels[offset + 1] = uint8_t(118 + (y / 24) % 20);
        }
    }
}

bool save(const std::string &path, const std::vector<uint8_t> &pixels) {
    FILE *file = std::fopen(path.c_str(), "wb");
    if (!file) return false;
    const bool written = std::fwrite(pixels.data(), 1, pixels.size(), file) == pixels.size();
    return std::fclose(file) == 0 && written;
}

bool run(const char *directory, uint32_t width, uint32_t height, bool lowLight) {
    std::vector<Frame> sources;
    sources.reserve(5);
    const unsigned count = lowLight ? 5 : 3;
    std::vector<uint64_t> sourceHashes;
    QCamera3HdrAlgo::Image images[5] = {};
    for (unsigned i = 0; i < count; ++i) {
        sources.emplace_back(width, height);
        const float gain = lowLight ? (i == 4 ? .25f : 1.f) :
                (i == 1 ? .5f : i == 2 ? 2.f : 1.f);
        fill(sources.back(), gain, lowLight ? i + 1 : 0);
        images[i] = sources.back().image;
        sourceHashes.push_back(hash(sources.back().pixels));
    }
    Frame output(width, height);
    std::memcpy(output.pixels.data(), sources[0].pixels.data(), output.pixels.size());
    const uint64_t baseHash = hash(output.pixels);
    int32_t invalidDrcStatus = 0;
    if (lowLight) {
        invalidDrcStatus = QCamera3LowLightHdrAlgo::process(images, output.image, 800, 0.f, 0);
        if (invalidDrcStatus != -EINVAL || hash(output.pixels) != baseHash) return false;
    }
    const auto start = std::chrono::steady_clock::now();
    // ISO and DRC are explicit synthetic test parameters, not camera metadata.
    const int32_t status = lowLight ?
            QCamera3LowLightHdrAlgo::process(images, output.image, 800, 1.f, 0) :
            QCamera3HdrAlgo::process(images, output.image, false);
    const double elapsedMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
    size_t changed = 0;
    double squaredError = 0;
    const size_t ySize = size_t(width) * height;
    for (size_t i = 0; i < ySize; ++i) {
        const int difference = int(output.pixels[i]) - sources[0].pixels[i];
        changed += difference != 0;
        squaredError += difference * difference;
    }
    bool inputsUnchanged = true;
    for (unsigned i = 0; i < count; ++i)
        inputsUnchanged &= hash(sources[i].pixels) == sourceHashes[i];
    const char *algorithm = lowLight ? "llhdr" : "hdr";
    const std::string prefix = std::string(directory) + "/" + algorithm;
    const bool saved = save(prefix + "-base.nv21", sources[0].pixels) &&
            save(prefix + "-output.nv21", output.pixels);
    std::printf("{\"algorithm\":\"%s\",\"synthetic\":true,\"width\":%u,\"height\":%u,"
            "\"status\":%d,\"elapsedMs\":%.3f,\"changedLuma\":%zu,\"lumaMseFromBase\":%.6f,"
            "\"baseHash\":\"%016llx\",\"outputHash\":\"%016llx\","
            "\"inputsUnchanged\":%s,\"saved\":%s,\"syntheticIso\":%d,"
            "\"syntheticDrcGain\":%.1f,\"invalidDrcStatus\":%d}\n",
            algorithm, width, height, status, elapsedMs, changed, squaredError / ySize,
            static_cast<unsigned long long>(baseHash), static_cast<unsigned long long>(hash(output.pixels)),
            inputsUnchanged ? "true" : "false", saved ? "true" : "false",
            lowLight ? 800 : 0, lowLight ? 1.f : 0.f, invalidDrcStatus);
    std::fflush(stdout);
    return status == 0 && inputsUnchanged && saved;
}
} // namespace

int main(int argc, char **argv) {
    if (argc != 5) return 2;
    const long width = std::strtol(argv[2], nullptr, 10);
    const long height = std::strtol(argv[3], nullptr, 10);
    if (width < 64 || height < 64 || width > 8192 || height > 8192 ||
            (width & 1) || (height & 1)) return 2;
    const bool hdr = std::strcmp(argv[4], "hdr") == 0;
    const bool llhdr = std::strcmp(argv[4], "llhdr") == 0;
    const bool both = std::strcmp(argv[4], "both") == 0;
    if (!hdr && !llhdr && !both) return 2;
    bool success = true;
    if (hdr || both) success = run(argv[1], uint32_t(width), uint32_t(height), false);
    if (llhdr || both) success = run(argv[1], uint32_t(width), uint32_t(height), true) && success;
    return success ? 0 : 1;
}
