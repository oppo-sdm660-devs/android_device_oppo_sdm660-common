#include "QCamera3FakeStreamRules.h"

#include <cassert>
#include <cstdio>

using QCamera3FakeStreamRules::isFakeStreamOnly;

static bool fake() {
    return isFakeStreamOnly(1, 0, 0x22, 320, 240, 0, 0x800, 0);
}

int main() {
    // AOSP Camera3FakeStream-only configuration.
    assert(fake());

    // Ordinary 320x240 IMPLEMENTATION_DEFINED preview without HW_COMPOSER.
    assert(!isFakeStreamOnly(1, 0, 0x22, 320, 240, 0, 0x900, 0));
    // Extra usage bits are not the AOSP fake stream.
    assert(!isFakeStreamOnly(1, 0, 0x22, 320, 240, 0, 0x800 | 0x100, 0));
    // Wrong geometry/format/rotation/data space.
    assert(!isFakeStreamOnly(1, 0, 0x22, 1840, 1380, 0, 0x800, 0));
    assert(!isFakeStreamOnly(1, 0, 0x22, 3840, 2160, 0, 0x800, 0));
    assert(!isFakeStreamOnly(1, 0, 0x23, 320, 240, 0, 0x800, 0));
    assert(!isFakeStreamOnly(1, 0, 0x22, 320, 240, 90, 0x800, 0));
    assert(!isFakeStreamOnly(1, 0, 0x22, 320, 240, 0, 0x800, 0x1000));
    // Input/bidirectional and multi-stream configurations are not fake-only.
    assert(!isFakeStreamOnly(1, 1, 0x22, 320, 240, 0, 0x800, 0));
    assert(!isFakeStreamOnly(2, 0, 0x22, 320, 240, 0, 0x800, 0));

    // Real preview+video/photo combinations remain outside the fake path.
    assert(!isFakeStreamOnly(2, 0, 0x22, 1840, 1380, 0, 0x900, 0));
    assert(!isFakeStreamOnly(3, 0, 0x22, 1840, 1380, 0, 0x900, 0));

    std::puts("fake stream rules: PASS");
    return 0;
}
