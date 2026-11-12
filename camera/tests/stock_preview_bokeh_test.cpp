/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3StockPreviewBokeh.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <errno.h>
#include <vector>

using Bokeh = qcamera::QCamera3StockPreviewBokeh;
struct Frame {
    std::vector<uint8_t> bytes;
    Bokeh::Image image;
    Frame(unsigned width, unsigned height, uint8_t chroma) :
            bytes((width + 16) * (height + 6) * 3 / 2 + 32, 0xac),
            image{bytes.data(), bytes.size(), width, height, 7,
                size_t(width + 16) * (height + 4) + 11, width + 16, width + 16,
                height + 4, height / 2 + 2} {
        for (unsigned y = 0; y < height; ++y)
            memset(bytes.data() + image.yOffset + y * image.yStride, 55, width);
        for (unsigned y = 0; y < height / 2; ++y)
            memset(bytes.data() + image.uvOffset + y * image.uvStride, chroma, width);
    }
};
Bokeh::Calibration calibration() {
    Bokeh::Calibration value;
    value.source = Bokeh::Calibration::MODULE_OTP;
    const float focal = 400;
    const int16_t width = 64, height = 48;
    memcpy(value.data + 0xc, &focal, 4); memcpy(value.data + 0x28, &focal, 4);
    memcpy(value.data + 0x10, &width, 2); memcpy(value.data + 0x12, &height, 2);
    memcpy(value.data + 0x2c, &width, 2); memcpy(value.data + 0x2e, &height, 2);
    return value;
}
int main() {
    void *library = dlopen("libarcsoft_dualcam_refocus_preview.so", RTLD_NOW);
    assert(library);
    auto mode = reinterpret_cast<void (*)(int)>(dlsym(library, "fake_preview_mode"));
    auto cancelInCore = reinterpret_cast<void (*)(std::atomic<bool> *)>(dlsym(library, "fake_preview_cancel"));
    auto inits = reinterpret_cast<int (*)()>(dlsym(library, "fake_preview_inits"));
    auto uninits = reinterpret_cast<int (*)()>(dlsym(library, "fake_preview_uninits"));
    auto processes = reinterpret_cast<int (*)()>(dlsym(library, "fake_preview_processes"));
    assert(mode && cancelInCore && inits && uninits && processes);
    assert(Bokeh::isAvailable());
    Bokeh backend;
    const Bokeh::Config config{{11, 22}, {64, 32}, {48, 24}, {128, 64}, {96, 48},
            270, 2000000};
    Bokeh::Pair pair{{11, 22}, {101, 99}, {100000000, 101000000}};
    Frame wide(64, 48, 91), tele(32, 24, 93), dst(64, 48, 99);
    const auto originalWide = wide.bytes, originalTele = tele.bytes, originalDst = dst.bytes;
    const Bokeh::Focus focus{32, 24, 70};
    Bokeh::Result result;
    assert(backend.process(pair, wide.image, tele.image, dst.image, focus, result) == -ENODEV);
    for (int failure : {1, 2, 3}) {
        mode(failure);
        assert(backend.configure(config, calibration()) == -EIO);
        assert(backend.process(pair, wide.image, tele.image, dst.image, focus, result) == -ENODEV);
    }
    mode(0);
    assert(backend.configure(config, calibration()) == 0);
    const int initialized = inits();
    for (int i = 0; i < 5; ++i) {
        assert(backend.process(pair, wide.image, tele.image, dst.image, focus, result) == -ENODATA);
        assert(!result.applied && result.coreStatus == 0 && result.effectStatus == 42);
        assert(dst.bytes == originalDst);
    }
    assert(backend.process(pair, wide.image, tele.image, dst.image, focus, result) == 0);
    assert(result.applied && result.source.frameNumbers[0] == 101 && result.source.frameNumbers[1] == 99);
    assert(dst.bytes[dst.image.yOffset] == (55 ^ 0x40));
    assert(dst.bytes[dst.image.yOffset + dst.image.width] == 0xac);
    assert(dst.bytes[dst.image.uvOffset] == 91 && dst.bytes[0] == 0xac);
    assert(wide.bytes == originalWide && tele.bytes == originalTele && inits() == initialized);
    dst.bytes = originalDst;
    for (int failure : {4, 5, 6}) {
        mode(failure);
        assert(backend.process(pair, wide.image, tele.image, dst.image, focus, result) ==
                (failure == 4 ? -ENODATA : failure == 5 ? -EIO : -EPROTO));
        assert(!result.applied && dst.bytes == originalDst);
    }
    mode(0);
    const int count = processes();
    auto invalidPair = pair;
    invalidPair.cameraHandles[0] = 22;
    assert(backend.process(invalidPair, wide.image, tele.image, dst.image, focus, result) == -EINVAL);
    invalidPair = pair; invalidPair.timestampsNs[1] += 3000000;
    assert(backend.process(invalidPair, wide.image, tele.image, dst.image, focus, result) == -EINVAL);
    invalidPair = pair; invalidPair.timestampsNs[0] = 0;
    assert(backend.process(invalidPair, wide.image, tele.image, dst.image, focus, result) == -EINVAL);
    assert(backend.process(pair, wide.image, tele.image, dst.image, {64, 24, 70}, result) == -EINVAL);
    assert(backend.process(pair, wide.image, tele.image, dst.image, {32, 24, 0}, result) == -ENODATA);
    auto badImage = wide.image; badImage.length = 10;
    assert(backend.process(pair, badImage, tele.image, dst.image, focus, result) == -EINVAL);
    std::atomic<bool> cancelled{true};
    assert(backend.process(pair, wide.image, tele.image, dst.image, focus, result, &cancelled) == -ECANCELED);
    assert(processes() == count);
    cancelled.store(false); cancelInCore(&cancelled);
    assert(backend.process(pair, wide.image, tele.image, dst.image, focus, result, &cancelled) == -ECANCELED);
    assert(!result.applied && dst.bytes == originalDst);
    cancelInCore(nullptr);
    backend.reset();
    auto badConfig = config; badConfig.activeHeights[0] = 0;
    assert(backend.configure(badConfig, calibration()) == -EINVAL);
    auto badCalibration = calibration(); badCalibration.data[0xc] = 0; badCalibration.data[0xd] = 0;
    badCalibration.data[0xe] = 0; badCalibration.data[0xf] = 0;
    assert(backend.configure(config, badCalibration) == -EINVAL);
    assert(inits() - uninits() == 2); // Init failure and null handle have nothing to uninit.
    assert(backend.configure(config, calibration()) == 0);
    assert(backend.process(pair, wide.image, tele.image, dst.image, focus, result) == -ENODATA);
    backend.reset();
    for (const auto size : {std::pair<unsigned, unsigned>{1280, 720}, {1920, 1080}}) {
        auto large = config;
        large.imageWidths[0] = large.imageWidths[1] = size.first;
        large.imageHeights[0] = large.imageHeights[1] = size.second;
        large.activeWidths[0] = large.activeWidths[1] = size.first * 2;
        large.activeHeights[0] = large.activeHeights[1] = size.second * 2;
        Frame a(size.first, size.second, 91), b(size.first, size.second, 93), out(size.first, size.second, 99);
        assert(backend.configure(large, calibration()) == 0);
        for (int i = 0; i < 6; ++i)
            assert(backend.process(pair, a.image, b.image, out.image,
                    {int32_t(size.first / 2), int32_t(size.second / 2), 70}, result) == (i < 5 ? -ENODATA : 0));
    }
    auto nonFourThree = config;
    nonFourThree.imageWidths[0] = nonFourThree.imageWidths[1] = 1280;
    nonFourThree.imageHeights[0] = nonFourThree.imageHeights[1] = 960;
    nonFourThree.activeWidths[0] = 4608; nonFourThree.activeHeights[0] = 3456;
    nonFourThree.activeWidths[1] = 2592; nonFourThree.activeHeights[1] = 1940;
    Frame a(1280, 960, 91), b(1280, 960, 93), out(1280, 960, 99);
    assert(backend.configure(nonFourThree, calibration()) == 0);
    for (int i = 0; i < 6; ++i)
        assert(backend.process(pair, a.image, b.image, out.image, {640, 480, 70}, result) ==
                (i < 5 ? -ENODATA : 0));
    backend.reset();
    dlclose(library);
    std::puts("stock preview bokeh tests: all checks passed");
}
