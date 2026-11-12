/* SPDX-License-Identifier: BSD-3-Clause */
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <errno.h>

constexpr int NO_ERROR = 0, OPPO_NATIVE_FRONT_BEAUTY = 10, OPPO_NATIVE_SCENE = 9;
constexpr uint32_t QCAMERA3_NATIVE_ALGORITHM_PROCESSING_ENABLE = 1;
constexpr uint32_t QCAMERA3_NATIVE_ALGORITHM_FRONT_BEAUTY_STRENGTH = 2;
constexpr uint32_t QCAMERA3_NATIVE_ALGORITHM_SCENE_STRENGTH = 3;
constexpr uint32_t QCAMERA3_NATIVE_ALGORITHM_PREVIEW_BOKEH_STRENGTH = 4;
constexpr uint32_t ANDROID_REQUEST_AVAILABLE_SESSION_KEYS = 5;
struct CameraMetadata {
    struct Entry { struct { const int32_t *i32; } data; size_t count; };
    std::map<uint32_t, std::vector<int32_t>> values;
    Entry find(uint32_t key) const {
        auto it = values.find(key);
        return it == values.end() ? Entry{{nullptr}, 0} : Entry{{it->second.data()}, it->second.size()};
    }
};
struct camera3_stream_t { int id; };
struct cam_dimension_t { int32_t width, height; };
struct cam_frame_len_offset_t {
    int num_planes = 2;
    struct Plane { uint32_t stride_in_bytes = 0, stride = 0, len = 0, offset = 0; } mp[2];
};
struct mm_camera_buf_def_t { void *buffer; size_t frame_len; timespec ts; uint32_t buf_idx; };
struct QCamera3StreamMem {
    int status = 0, calls = 0;
    int32_t cleanCache(uint32_t) { ++calls; return status; }
};
struct QCamera3OppoPlatformFrame {
    uint8_t *data; size_t length; int width, height, stride, slice, beautyStrength, sceneStrength;
};
class QCamera3OppoPlatformHwi {
public:
    int adjustCalls = 0, beautyCalls = 0, sceneCalls = 0, lastIso = 0;
    int adjustStatus = 0, beautyStatus = 0, sceneStatus = 0;
    std::atomic<bool> *cancelInCore = nullptr;
    static QCamera3OppoPlatformHwi &get() { static QCamera3OppoPlatformHwi instance; return instance; }
    int processImageAdjustFrame(QCamera3OppoPlatformFrame &frame, bool, int iso, bool realtime) {
        ++adjustCalls; lastIso = iso; assert(realtime);
        assert(frame.data[size_t(frame.width) * frame.height] == 90);
        assert(frame.data[size_t(frame.width) * frame.height + 1] == 80);
        if (!adjustStatus) frame.data[0] = 100;
        if (cancelInCore) cancelInCore->store(true);
        return adjustStatus;
    }
    int processExplicitFrame(int mode, QCamera3OppoPlatformFrame &frame, bool) {
        if (mode == 10) {
            ++beautyCalls;
            if (!beautyStatus) frame.data[1] = 101;
            return beautyStatus;
        }
        assert(mode == 9); ++sceneCalls;
        if (!sceneStatus) frame.data[2] = 102;
        return sceneStatus;
    }
};
uint32_t get_main_camera_handle(uint32_t handle) { return handle & 0xffff; }
class QCamera3HardwareInterface {
public:
    struct AcceptedRequest {
        CameraMetadata settings;
        camera3_stream_t *realtimeOutput = nullptr;
        camera3_stream_t *previewBokehOutput = nullptr;
    };
    struct RealtimeSource { int64_t timestampNs; int32_t sensitivity; };
    struct Record { int32_t frame, stage, status, applied; };
    struct Mapping { int getFrameworkFrameNumber(uint32_t in, uint32_t &out) { out = in - 100; return 0; } } _orchestrationDb;
    struct Handle { uint32_t camera_handle = 0x220011; } handle;
    Handle *mCameraHandle = &handle;
    std::mutex mAsyncLock, mRealtimeSourceLock;
    std::condition_variable mRealtimeSourceCondition;
    std::map<uint32_t, std::shared_ptr<AcceptedRequest>> mAcceptedRequests;
    std::map<uint32_t, RealtimeSource> mRealtimeSources;
    std::atomic<bool> mAsyncCancelled{false};
    std::vector<Record> records;
    bool front = false, dual = false;
    bool mRealtimeLinearConfigured = true;
    bool isFrontCamera() const { return front; }
    bool isDualCamera() const { return dual; }
    void noteNativeStage(uint32_t frame, int32_t stage, int32_t status, int32_t applied) {
        records.push_back({int32_t(frame), stage, status, applied});
    }
    void cacheRealtimeSource(uint32_t, uint32_t, int64_t, int32_t);
    void noteRealtimeFailure(uint32_t, const camera3_stream_t *, int32_t, bool = true);
    bool processRealtimeFrame(uint32_t, mm_camera_buf_def_t *, const cam_dimension_t &,
            const cam_frame_len_offset_t &, bool, const camera3_stream_t *, QCamera3StreamMem * = nullptr);
};
#include "requested.inc"
#include "session.inc"
#include "cache.inc"
#include "failure.inc"
#include "process.inc"

struct Fixture {
    QCamera3HardwareInterface hal;
    camera3_stream_t preview{1}, video{2};
    std::shared_ptr<QCamera3HardwareInterface::AcceptedRequest> owner =
            std::make_shared<QCamera3HardwareInterface::AcceptedRequest>();
    std::vector<uint8_t> pixels = std::vector<uint8_t>(96, 0xaa);
    cam_dimension_t dim{8, 4};
    cam_frame_len_offset_t offset;
    mm_camera_buf_def_t buffer{pixels.data(), pixels.size(), {1, 0}, 3};
    QCamera3StreamMem memory;
    Fixture(bool enhanced = true) {
        QCamera3OppoPlatformHwi::get() = {};
        owner->realtimeOutput = enhanced ? &video : nullptr;
        if (enhanced) owner->settings.values[QCAMERA3_NATIVE_ALGORITHM_PROCESSING_ENABLE] = {1};
        hal.mAcceptedRequests[1] = owner;
        offset.mp[0] = {12, 12, 60, 3};
        offset.mp[1] = {12, 12, 24, 2};
        for (int row = 0; row < 2; ++row)
            for (int x = 0; x < 8; x += 2) {
                pixels[62 + row * 12 + x] = 80;
                pixels[62 + row * 12 + x + 1] = 90;
            }
    }
    bool process(const camera3_stream_t *output = nullptr) {
        return hal.processRealtimeFrame(101, &buffer, dim, offset, true, output ? output : &video, &memory);
    }
    void metadata(uint32_t camera = 0x11, uint32_t frame = 101, int64_t timestamp = 1000000000) {
        hal.cacheRealtimeSource(frame, camera, timestamp, 200);
    }
};
int main() {
    {
        CameraMetadata settings, capabilities;
        capabilities.values[ANDROID_REQUEST_AVAILABLE_SESSION_KEYS] = {1, 2, 3, 4};
        bool linear = true; int32_t previewBokeh = 10;
        assert(oppoRealtimeSession(settings, capabilities, linear, previewBokeh));
        assert(!linear && previewBokeh == 0);
        settings.values[1] = {0}; settings.values[2] = {0}; settings.values[3] = {0};
        assert(oppoRealtimeSession(settings, capabilities, linear, previewBokeh) && !linear);
        settings.values[1] = {1};
        assert(oppoRealtimeSession(settings, capabilities, linear, previewBokeh) && linear);
        settings.values[4] = {60};
        assert(oppoRealtimeSession(settings, capabilities, linear, previewBokeh) && previewBokeh == 60);
        settings.values[1] = {2};
        assert(!oppoRealtimeSession(settings, capabilities, linear, previewBokeh));
        settings.values[1] = {1, 1};
        assert(!oppoRealtimeSession(settings, capabilities, linear, previewBokeh));
        settings.values[1] = {1}; capabilities.values[ANDROID_REQUEST_AVAILABLE_SESSION_KEYS] = {2, 3, 4};
        assert(!oppoRealtimeSession(settings, capabilities, linear, previewBokeh));
    }
    {
        Fixture f(false);
        const auto before = f.pixels;
        assert(!oppoRealtimeRequested(f.owner->settings));
        assert(!f.process() && f.pixels == before && f.hal.records.empty());
        assert(QCamera3OppoPlatformHwi::get().adjustCalls == 0 && f.memory.calls == 0);
    }
    {
        Fixture f;
        f.owner->previewBokehOutput = &f.preview;
        f.owner->settings.values[QCAMERA3_NATIVE_ALGORITHM_PREVIEW_BOKEH_STRENGTH] = {60};
        f.hal.noteRealtimeFailure(101, &f.preview, -EIO);
        assert(f.hal.records.size() == 1 && f.hal.records[0].stage == 23 &&
                f.hal.records[0].status == -EIO && f.hal.records[0].applied == 0);
        f.hal.noteRealtimeFailure(101, &f.video, -ENOTSUP);
        assert(f.hal.records.size() == 2 && f.hal.records[1].stage == 30);
    }
    {
        Fixture f;
        f.owner->realtimeOutput = f.owner->previewBokehOutput = &f.preview;
        f.owner->settings.values[QCAMERA3_NATIVE_ALGORITHM_PREVIEW_BOKEH_STRENGTH] = {60};
        f.hal.dual = true;
        f.hal.noteNativeStage(101, 23, 0, 1);
        assert(!f.process(&f.preview));
        assert(f.hal.records.size() == 2 && f.hal.records[0].stage == 23 &&
                f.hal.records[0].applied == 1 && f.hal.records[1].stage == 30 &&
                f.hal.records[1].status == -ENOTSUP);
    }
    {
        Fixture f;
        assert(oppoRealtimeRequested(f.owner->settings));
        f.metadata(); assert(f.process());
        assert(f.pixels[3] == 100 && f.pixels[11] == 0xaa && f.pixels[62] == 80);
        assert(f.memory.calls == 1 && QCamera3OppoPlatformHwi::get().lastIso == 200);
        assert(f.hal.records.size() == 1 && f.hal.records[0].stage == 30 && f.hal.records[0].applied == 1);
    }
    {
        Fixture f;
        auto processing = std::async(std::launch::async, [&] { return f.process(); });
        assert(processing.wait_for(std::chrono::milliseconds(5)) == std::future_status::timeout);
        f.metadata(); assert(processing.get());
        assert(f.hal.records[0].applied == 1);
    }
    {
        Fixture f;
        const auto before = f.pixels;
        f.metadata(0x22); // Synchronized auxiliary metadata cannot supply main ISO.
        assert(f.hal.mRealtimeSources.empty());
        f.metadata(0x11, 100); // The preceding exposure cannot supply this frame.
        assert(!f.process() && f.pixels == before);
        assert(f.hal.records[0].status == -ENODATA && f.hal.records[0].applied == 0);
        assert(QCamera3OppoPlatformHwi::get().adjustCalls == 0);
    }
    {
        Fixture f;
        f.metadata(0x11, 101, 1000020000);
        assert(!f.process() && f.hal.records[0].status == -ENODATA);
    }
    {
        Fixture f;
        f.metadata();
        assert(!f.process(&f.preview) && f.hal.records.empty());
        assert(f.process() && f.hal.records.size() == 1); // Video target owns the stage.
    }
    {
        Fixture f;
        f.hal.front = true;
        f.owner->settings.values[QCAMERA3_NATIVE_ALGORITHM_FRONT_BEAUTY_STRENGTH] = {50};
        f.owner->settings.values[QCAMERA3_NATIVE_ALGORITHM_SCENE_STRENGTH] = {35};
        QCamera3OppoPlatformHwi::get().sceneStatus = -EIO;
        f.metadata(); assert(f.process());
        assert(f.hal.records.size() == 3);
        assert(f.hal.records[1].stage == 10 && f.hal.records[1].applied == 1);
        assert(f.hal.records[2].stage == 9 && f.hal.records[2].status == -EIO && f.hal.records[2].applied == 0);
    }
    {
        Fixture f;
        f.memory.status = -EIO; f.metadata();
        assert(!f.process() && f.hal.records[0].status == -EIO && f.hal.records[0].applied == 0);
    }
    {
        Fixture f;
        f.hal.dual = true; f.metadata();
        assert(!f.process() && f.hal.records[0].status == -ENOTSUP);
    }
    {
        Fixture f;
        f.hal.mRealtimeLinearConfigured = false; f.metadata();
        assert(!f.process() && f.hal.records[0].status == -ENOTSUP);
    }
    {
        Fixture f;
        f.buffer.frame_len = 8; f.metadata();
        assert(!f.process() && f.hal.records[0].status == -EINVAL);
    }
    {
        Fixture f;
        const auto before = f.pixels;
        f.metadata(); QCamera3OppoPlatformHwi::get().cancelInCore = &f.hal.mAsyncCancelled;
        assert(!f.process() && f.pixels == before && f.hal.records[0].status == -ECANCELED);
    }
    std::puts("realtime frame tests: all checks passed");
}
