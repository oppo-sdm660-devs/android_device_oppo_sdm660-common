/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3AlgoTrace.h"
#include "QCamera3NativeAlgorithms.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <mutex>
#include <cstring>
#include <cerrno>
using qcamera::QCamera3AlgoTrace;
using namespace qcamera;
constexpr int NO_ERROR = 0;
#define MIN(a,b) std::min((a),(b))
#define LOGI(...) ((void)0)
struct Mutex : std::mutex {
    using Autolock = std::lock_guard<Mutex>;
};
class QCamera3HardwareInterface {
public:
#include "state.inc"
    void noteNativeTrace(uint32_t, const char *, int32_t);
    void noteNativeStage(uint32_t, int32_t, int32_t, int32_t);
    void noteOcclusionResult(uint32_t, int32_t, int32_t, int32_t, int32_t);
    bool getNativeStageRecords(uint32_t, int32_t *, size_t, int32_t &);
    bool getOcclusionResult(uint32_t, int32_t *, size_t);
    bool getNativeTrace(uint32_t, char *, size_t, int32_t &);
    int getFrameworkFrameNumber(uint32_t in, uint32_t &out) { out = in + 100; return 0; }
};
#include "functions.inc"
struct jpeg_settings_t {
    uint32_t framework_frame_number = 129;
    bool framework_frame_number_valid = true;
    char image_desc[128] = {};
    bool image_desc_valid = false, native_trace_valid = false;
    int32_t native_still_mode = OPPO_NATIVE_SUPERZOOM;
    QCamera3AlgoTrace native_trace;
};
#include "trace.inc"
int main() {
    QCamera3HardwareInterface hal;
    int32_t records[32], count, occlusion[4];
    char trace[513];
    int32_t mode;
    for (uint32_t frame = 1; frame < 41; ++frame) {
        hal.noteNativeTrace(frame, "HDR", 0);
        assert(!hal.getOcclusionResult(frame, occlusion, 4));
        assert(!hal.getNativeStageRecords(frame, records, 32, count));
        hal.noteNativeStage(frame, int32_t(frame % 22 + 1), -22, 0);
        hal.noteOcclusionResult(frame, 1, int32_t(frame), 7, 0);
        hal.noteNativeTrace(frame, "HDR;Occlusion", 0);
        assert(hal.getNativeStageRecords(frame, records, 32, count));
        assert(count == 1 && records[0] == int32_t(frame % 22 + 1));
        assert(records[1] == -22 && records[2] == 0 && records[3] == int32_t(frame));
    }
    for (uint32_t frame = 41; frame < 61; ++frame) {
        hal.noteNativeStage(frame, 22, 0, 1);
        assert(!hal.getOcclusionResult(frame, occlusion, 4));
        assert(hal.getNativeStageRecords(frame, records, 32, count));
        assert(count == 1 && records[3] == int32_t(frame));
        assert(!hal.getNativeTrace(frame, trace, sizeof(trace), mode) && trace[0] == 0 && mode == 0);
    }
    for (uint32_t frame = 61; frame < 81; ++frame) {
        hal.noteOcclusionResult(frame, 0, int32_t(frame), 7, 0);
        assert(!hal.getNativeStageRecords(frame, records, 32, count));
        assert(hal.getOcclusionResult(frame, occlusion, 4) && occlusion[1] == int32_t(frame));
        assert(!hal.getNativeTrace(frame, trace, sizeof(trace), mode) && trace[0] == 0 && mode == 0);
    }
    assert(!hal.getNativeStageRecords(1, records, 32, count));
    jpeg_settings_t jpeg;
    nativeAlgorithmTrace(&jpeg, "VDSuperZoom2x", 0, 129, 4, &hal, 1);
    nativeAlgorithmTrace(&jpeg, "FlashSequence", -61, 129, 0, &hal, 0, OPPO_STAGE_FLASH_SEQUENCE);
    nativeAlgorithmTrace(&jpeg, "SceneBrightness", -22, 129, 0, &hal, 0, OPPO_STAGE_SCENE_BRIGHTNESS);
    nativeAlgorithmTrace(&jpeg, "Occlusion", -22, 129, 1, &hal, 0, OPPO_STAGE_OCCLUSION);
    assert(hal.getNativeStageRecords(129, records, 32, count) && count == 4);
    assert(records[0] == 22 && records[1] == 0 && records[2] == 1);
    assert(records[4] == 1001 && records[8] == 1002 && records[12] == 1003);
    assert(hal.getNativeTrace(129, trace, sizeof(trace), mode) && mode == 22);
    std::puts("native trace tests: 80 frames across ring slot reuse passed");
    std::puts("native trace tests: diagnostic failures retain distinct stages and fusion success");
}
