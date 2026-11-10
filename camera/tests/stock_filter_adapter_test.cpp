/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3StockFilter.h"
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <vector>
using qcamera::QCamera3StockFilter;
int main(int argc, char **argv) {
    assert(argc == 2);
    void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    assert(lib);
    auto setMode = reinterpret_cast<void (*)(int, std::atomic<bool> *)>(dlsym(lib, "stockFilterTestMode"));
    assert(setMode && QCamera3StockFilter::isAvailable());
    assert(QCamera3StockFilter::supportsType(0));
    for (int type = 2; type <= 5; ++type) assert(!QCamera3StockFilter::supportsType(type));
    assert(!QCamera3StockFilter::supportsType(21));
    std::vector<uint8_t> rgba(64, 0x3c), before = rgba;
    assert(QCamera3StockFilter::processRgba(rgba.data(), rgba.size(), 2, 3, 12, 0, 2) == 0);
    for (size_t p = 0; p < rgba.size(); ++p) {
        bool processed = p < 36 && p % 12 < 8 && p % 4 == 0;
        assert(rgba[p] == (processed ? uint8_t(before[p] ^ 0xff) : before[p]));
    }
    for (int mode = 1; mode <= 2; ++mode) {
        rgba = before;
        std::atomic<bool> cancelled{false};
        setMode(mode, &cancelled);
        assert(QCamera3StockFilter::processRgba(rgba.data(), rgba.size(), 2, 3, 12, 0,
                0, &cancelled) == (mode == 1 ? -EIO : -ECANCELED));
        assert(rgba == before);
    }
    setMode(0, nullptr);
    std::vector<uint8_t> nv21(80, 0xa5);
    QCamera3StockFilter::Image im = {nv21.data(), nv21.size(), 4, 4, 3, 45, 7, 9, 4, 2};
    for (unsigned row = 0; row < 4; ++row)
        for (unsigned col = 0; col < 4; ++col) nv21[3 + row * 7 + col] = 100;
    for (unsigned row = 0; row < 2; ++row)
        for (unsigned col = 0; col < 4; ++col) nv21[45 + row * 9 + col] = 128;
    const auto nvBefore = nv21;
    assert(QCamera3StockFilter::process(im, 0) == 0);
    for (size_t p = 0; p < nv21.size(); ++p) {
        bool y = p >= 3 && p < 31 && (p - 3) % 7 < 4;
        bool uv = p >= 45 && p < 63 && (p - 45) % 9 < 4;
        if (!y && !uv) assert(nv21[p] == nvBefore[p]);
        if (y) assert(nv21[p] == 117);
    }
    nv21 = nvBefore;
    setMode(1, nullptr);
    assert(QCamera3StockFilter::process(im, 0) == -EIO);
    assert(nv21 == nvBefore);
    im.length = 48;
    assert(QCamera3StockFilter::process(im, 0) == -EINVAL);
    assert(QCamera3StockFilter::process(im, 2) == -ENOTSUP);
    dlclose(lib);
}
