#!/usr/bin/env python3
"""Exercise the production request-to-duration boundary without waiting for a sensor."""
import os
from pathlib import Path
import subprocess
import tempfile

here = Path(__file__).resolve().parent
source = (here.parent / 'QCamera2/HAL3/QCamera3HWI.cpp').read_text()
start = source.index('static int64_t oppoManualCaptureDurationNs(')
end = source.index('\nstatic bool oppoPanoramaSupported', start)
fixture = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <map>
enum { ANDROID_CONTROL_AE_MODE, ANDROID_SENSOR_EXPOSURE_TIME, ANDROID_SENSOR_FRAME_DURATION };
constexpr int ANDROID_CONTROL_AE_MODE_OFF = 0;
struct Entry {
    size_t count = 0;
    struct Data { const uint8_t *u8 = nullptr; const int64_t *i64 = nullptr; } data;
};
struct CameraMetadata {
    uint8_t ae = 0;
    std::map<int, int64_t> values;
    Entry find(int tag) const {
        if (tag == ANDROID_CONTROL_AE_MODE) return {1, {&ae, nullptr}};
        auto found = values.find(tag);
        return found == values.end() ? Entry{} : Entry{1, {nullptr, &found->second}};
    }
};
struct cam_capability_t { int64_t exposure_time_range[2] = {12000, 48008228030LL}; };
struct Limits {
    const cam_capability_t *cap;
    bool validExposure(int64_t value) const {
        return value >= cap->exposure_time_range[0] && value <= cap->exposure_time_range[1];
    }
};
static Limits oppoManualLimits(const cam_capability_t *cap) { return {cap}; }
'''
checks = r'''
int main() {
    cam_capability_t cap;
    CameraMetadata settings;
    settings.values[ANDROID_SENSOR_EXPOSURE_TIME] = 48008228030LL;
    assert(oppoManualCaptureDurationNs(settings, &cap) == 48008228030LL);
    assert(30000000000LL + 5 * oppoManualCaptureDurationNs(settings, &cap) == 270041140150LL);
    settings.ae = 1;
    assert(oppoManualCaptureDurationNs(settings, &cap) == 0);
    settings.ae = 0;
    settings.values[ANDROID_SENSOR_EXPOSURE_TIME] = 1000000000LL;
    settings.values[ANDROID_SENSOR_FRAME_DURATION] = 2000000000LL;
    assert(oppoManualCaptureDurationNs(settings, &cap) == 2000000000LL);
    settings.values[ANDROID_SENSOR_EXPOSURE_TIME] = -1;
    assert(oppoManualCaptureDurationNs(settings, &cap) == 0);
    settings.values[ANDROID_SENSOR_EXPOSURE_TIME] = INT64_MAX;
    assert(oppoManualCaptureDurationNs(settings, &cap) == 0);
    settings.values.erase(ANDROID_SENSOR_EXPOSURE_TIME);
    assert(oppoManualCaptureDurationNs(settings, &cap) == 0);
    assert(oppoManualCaptureDurationNs(settings, nullptr) == 0);
    puts("Capture duration production-boundary tests: PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='ocam-capture-duration-') as folder:
    path = Path(folder)
    (path / 'test.cpp').write_text(fixture + source[start:end] + checks)
    subprocess.run([os.environ.get('CXX', 'g++'), '-std=gnu++17', '-Wall', '-Wextra', '-Werror',
                    '-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined',
                    str(path / 'test.cpp'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
