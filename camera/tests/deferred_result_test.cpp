/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3RequestLifecycle.h"
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <atomic>

constexpr int NO_ERROR = 0, PARTIAL_RESULT_COUNT = 2, HAL_PIXEL_FORMAT_BLOB = 33;
constexpr uint32_t EMPTY_FRAMEWORK_FRAME_NUMBER = UINT32_MAX;
constexpr int OPPO_NATIVE_OFF = 0, OPPO_NATIVE_SUPERZOOM = 22;
constexpr int QCAMERA3_DIAGNOSTICS_STAGE_RECORDS = 1, QCAMERA3_NATIVE_ALGORITHM_NATIVE_TRACE = 2;
constexpr int QCAMERA3_NATIVE_ALGORITHM_APPLIED_NATIVE_MODE = 3, ANDROID_SCALER_CROP_REGION = 4;
constexpr int QCAMERA3_NATIVE_ALGORITHM_APPLIED_SUPER_ZOOM = 5, SENSOR_TIMESTAMP = 6, SUPER_ZOOM = 7;
constexpr int ANDROID_CONTROL_VIDEO_STABILIZATION_MODE = 8;
constexpr uint8_t ANDROID_CONTROL_VIDEO_STABILIZATION_MODE_OFF = 0;
constexpr uint8_t ANDROID_CONTROL_VIDEO_STABILIZATION_MODE_ON = 1;
constexpr int QCAMERA3_DIAGNOSTICS_VSTAB_PATH = 9, QCAMERA3_DIAGNOSTICS_VSTAB_REASON = 10;
constexpr int QCAMERA3_DIAGNOSTICS_VSTAB_CORE_STATUS = 11;
constexpr int QCAMERA3_DIAGNOSTICS_VSTAB_GYRO_COUNT = 12;
constexpr int QCAMERA3_DIAGNOSTICS_VSTAB_RENDERER = 13;
constexpr int QCAMERA3_DIAGNOSTICS_VSTAB_GPU_STATUS = 14;
#define LOGE(...) ((void)0)
#define LOGD(...) ((void)0)
struct QCamera3AlgoTrace { static constexpr size_t kMaxStages = 8, kMaxLength = 256; };
struct camera_metadata_t { std::map<int, std::vector<int32_t>> values; };
class CameraMetadata {
    camera_metadata_t data;
    mutable bool locked = false;
public:
    struct Entry { struct { const int32_t *i32; const uint8_t *u8; } data; size_t count; };
    CameraMetadata &operator=(const camera_metadata_t *p) { data = *p; return *this; }
    bool isEmpty() const { return data.values.empty(); }
    void clear() { data.values.clear(); }
    void update(int tag, const int32_t *p, size_t count) { data.values[tag] = {p, p + count}; }
    void update(int tag, const uint8_t *p, size_t count) { data.values[tag] = {p, p + count}; }
    Entry find(int tag) const {
        const auto it = data.values.find(tag);
        return it == data.values.end() ? Entry{{nullptr, nullptr}, 0} :
                Entry{{it->second.data(), reinterpret_cast<const uint8_t *>(it->second.data())}, it->second.size()};
    }
    Entry find(int tag) {
        return locked ? Entry{{nullptr, nullptr}, 0} : static_cast<const CameraMetadata &>(*this).find(tag);
    }
    const camera_metadata_t *getAndLock() const { locked = true; return &data; }
    void unlock(const camera_metadata_t *) const { locked = false; }
};
bool oppoSuperZoom(const CameraMetadata &settings) { return settings.find(SUPER_ZOOM).count != 0; }
using buffer_handle_t = int;
struct camera3_stream_t { int format; void *priv = nullptr; };
struct camera3_stream_buffer_t { camera3_stream_t *stream; buffer_handle_t *buffer; int status = 0; };
struct camera3_capture_result_t {
    uint32_t frame_number = 0;
    const camera_metadata_t *result = nullptr;
    uint32_t num_output_buffers = 0;
    const camera3_stream_buffer_t *output_buffers = nullptr;
    const camera3_stream_buffer_t *input_buffer = nullptr;
    uint32_t partial_result = 0;
};
struct Callback {
    void (*process_capture_result)(Callback *, const camera3_capture_result_t *);
    std::vector<camera_metadata_t> metadata;
    size_t buffers = 0;
    static void result(Callback *self, const camera3_capture_result_t *result) {
        if (result->result) self->metadata.push_back(*result->result);
        self->buffers += result->num_output_buffers;
    }
    Callback() : process_capture_result(result) {}
};
struct Mapping {
    int getFrameworkFrameNumber(uint32_t in, uint32_t &out) { out = in - 100; return 0; }
    void retireFrameworkFrameNumber(uint32_t) {}
};
class QCamera3MmsChannel {
public:
    struct VstabDiagnostics {
        int32_t path = 0, reason = 0, coreStatus = 0;
        int32_t gyroCount = 0, renderer = 0, gpuStatus = 0;
    } diagnostics;
    bool available = false;
    uint32_t frameNumber = 101, queriedFrame = UINT32_MAX;
    bool getVstabDiagnostics(uint32_t frame, VstabDiagnostics &result) {
        queriedFrame = frame;
        if (!available || frame != frameNumber) return false;
        result = diagnostics;
        return true;
    }
};
class QCamera3HardwareInterface {
public:
    struct AcceptedRequest : qcamera::QCamera3RequestLifecycle {
        CameraMetadata settings, deferredMetadata;
        bool deferFinalMetadata = false;
        int64_t captureWaitNs = 30000000000LL;
        camera3_stream_t *mmsOutput = nullptr;
        camera3_stream_t *realtimeOutput = nullptr;
        camera3_stream_t *previewBokehOutput = nullptr;
        std::vector<buffer_handle_t> handles;
        std::vector<camera3_stream_buffer_t> outputs;
    };
    std::mutex mAsyncLock;
    std::condition_variable mAsyncCondition;
    std::atomic<bool> mAsyncCancelled{false};
    std::map<uint32_t, std::shared_ptr<AcceptedRequest>> mAcceptedRequests;
    Mapping _orchestrationDb;
    Callback callbacks;
    Callback *mCallbackOps = &callbacks;
    int32_t stageId = 1, status = -22, applied = 0;
    bool hasStages = false;
    bool hasTrace = true;
    QCamera3MmsChannel mms;
    void orchestrateResult(camera3_capture_result_t *);
    std::shared_ptr<AcceptedRequest> recordAsyncResult(uint32_t, const camera3_capture_result_t &);
    int32_t waitForAsyncResult(uint32_t);
    bool getNativeStageRecords(uint32_t frame, int32_t *records, size_t, int32_t &count) {
        if (!hasStages) return false;
        records[0] = stageId; records[1] = status; records[2] = applied; records[3] = frame;
        count = 1; return true;
    }
    bool getNativeTrace(uint32_t, char *trace, size_t size, int32_t &mode) {
        if (!hasStages || !hasTrace) return false;
        std::snprintf(trace, size, "HDR status=%d", status);
        mode = applied ? stageId : 0; return true;
    }
};
#include "record.inc"
#include "wait.inc"
#include "result.inc"

struct Fixture {
    QCamera3HardwareInterface hal;
    camera3_stream_t blob{HAL_PIXEL_FORMAT_BLOB}, preview{35}, video{35, &hal.mms};
    buffer_handle_t handles[3] = {7, 8, 9};
    camera3_stream_buffer_t buffers[3] = {
        {&blob, &handles[0]}, {&preview, &handles[1]}, {&video, &handles[2]}};
    std::shared_ptr<QCamera3HardwareInterface::AcceptedRequest> owner;
    Fixture(bool deferred = true, bool zoom = false, bool mms = false, bool mixed = false,
            bool realtime = false) {
        owner = std::make_shared<QCamera3HardwareInterface::AcceptedRequest>();
        if ((mms || realtime) && !mixed) {
            owner->outputs = {buffers[2], buffers[1]};
            owner->handles = {handles[2], handles[1]};
        } else {
            owner->outputs.assign(buffers, buffers + (mixed ? 3 : 2));
            owner->handles.assign(handles, handles + (mixed ? 3 : 2));
        }
        owner->returned.resize(owner->outputs.size(), false);
        owner->submitting = true;
        owner->deferFinalMetadata = deferred;
        if (mms) {
            owner->mmsOutput = &video;
            const uint8_t on = ANDROID_CONTROL_VIDEO_STABILIZATION_MODE_ON;
            owner->settings.update(ANDROID_CONTROL_VIDEO_STABILIZATION_MODE, &on, 1);
        }
        if (realtime) owner->realtimeOutput = &video;
        if (zoom) {
            const int32_t on = 1, crop[] = {1160, 874, 2320, 1740};
            owner->settings.update(SUPER_ZOOM, &on, 1);
            owner->settings.update(ANDROID_SCALER_CROP_REGION, crop, 4);
        }
        owner->settings.getAndLock();
        hal.mAcceptedRequests[1] = owner;
    }
    void metadata() {
        CameraMetadata m;
        const int32_t timestamp = 1234567;
        m.update(SENSOR_TIMESTAMP, &timestamp, 1);
        camera3_capture_result_t result{101, m.getAndLock(), 0, nullptr, nullptr, 2};
        hal.orchestrateResult(&result);
    }
    void buffer(unsigned index, int status = 0) {
        buffers[index].status = status;
        camera3_capture_result_t result{101, nullptr, 1, &buffers[index], nullptr, 0};
        hal.orchestrateResult(&result);
    }
};
int main() {
    {
        Fixture f(false);
        assert(f.hal.waitForAsyncResult(1) == 0); // Ordinary single frame never waits for JPEG.
        f.metadata();
        assert(f.hal.callbacks.metadata.size() == 1);
    }
    {
        Fixture f;
        auto waiting = std::async(std::launch::async, [&] { return f.hal.waitForAsyncResult(1); });
        f.metadata(); f.buffer(1);
        assert(f.hal.callbacks.metadata.empty()); // Preview cannot release the final metadata.
        assert(waiting.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
        f.hal.hasStages = true;
        f.buffer(0);
        assert(waiting.get() == 0); // No framework callback acknowledgement is needed.
        assert(f.owner->settled() && !f.owner->complete());
        assert(f.hal.callbacks.metadata.size() == 1);
        const auto &m = f.hal.callbacks.metadata[0].values;
        assert(m.at(SENSOR_TIMESTAMP)[0] == 1234567);
        assert((m.at(QCAMERA3_DIAGNOSTICS_STAGE_RECORDS) == std::vector<int32_t>{1, -22, 0, 101}));
        f.metadata(); f.buffer(0); f.buffer(1);
        assert(f.hal.callbacks.metadata.size() == 1 && f.hal.callbacks.buffers == 2);
    }
    {
        Fixture f;
        f.hal.hasStages = true;
        f.buffer(0); f.buffer(1); f.metadata(); // JPEG may arrive before native final metadata.
        assert(f.owner->settled() && f.hal.callbacks.metadata.size() == 1);
    }
    {
        Fixture f;
        f.metadata(); f.owner->metadataDone = true; // ERROR_RESULT settles metadata, buffers still owed.
        f.buffer(0, 1); f.buffer(1);
        assert(f.owner->settled() && f.hal.callbacks.metadata.empty());
    }
    {
        Fixture f(false, true);
        f.hal.hasStages = true; f.hal.stageId = 22; f.hal.status = 0; f.hal.applied = 1;
        f.metadata(); f.buffer(1); f.buffer(0);
        const auto &m = f.hal.callbacks.metadata.at(0).values;
        assert((m.at(ANDROID_SCALER_CROP_REGION) == std::vector<int32_t>{1160, 874, 2320, 1740}));
        assert(m.at(QCAMERA3_NATIVE_ALGORITHM_APPLIED_SUPER_ZOOM)[0] == 1);
    }
    {
        Fixture f;
        auto waiting = std::async(std::launch::async, [&] { return f.hal.waitForAsyncResult(1); });
        assert(waiting.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
        f.hal.mAsyncCancelled.store(true); f.hal.mAsyncCondition.notify_all();
        assert(waiting.get() == 0);
    }
    {
        Fixture f(false, false, true);
        f.metadata(); f.buffer(1);
        assert(f.hal.callbacks.metadata.empty());
        f.hal.mms.available = true;
        f.hal.mms.diagnostics = {2, 0, 0, 24, 1, 0};
        f.buffer(2);
        assert(f.hal.callbacks.metadata.size() == 1 && f.owner->settled());
        const auto &m = f.hal.callbacks.metadata[0].values;
        assert(m.at(ANDROID_CONTROL_VIDEO_STABILIZATION_MODE)[0] == ANDROID_CONTROL_VIDEO_STABILIZATION_MODE_ON);
        assert(m.at(QCAMERA3_DIAGNOSTICS_VSTAB_PATH)[0] == 2);
        assert(m.at(QCAMERA3_DIAGNOSTICS_VSTAB_CORE_STATUS)[0] == 0);
        assert(m.at(QCAMERA3_DIAGNOSTICS_VSTAB_GYRO_COUNT)[0] == 24);
        assert(m.at(QCAMERA3_DIAGNOSTICS_VSTAB_RENDERER)[0] == 1);
        assert(m.at(QCAMERA3_DIAGNOSTICS_VSTAB_GPU_STATUS)[0] == 0);
        assert(m.at(SENSOR_TIMESTAMP)[0] == 1234567 && f.hal.mms.queriedFrame == 101);
        f.metadata(); f.buffer(2);
        assert(f.hal.callbacks.metadata.size() == 1 && f.hal.callbacks.buffers == 2);
    }
    {
        Fixture f(false, false, true);
        f.hal.mms.available = true;
        f.hal.mms.diagnostics = {2, 0, 0, 17, 0, -ENODEV};
        f.buffer(2); f.buffer(1);
        assert(f.hal.callbacks.metadata.empty());
        f.metadata();
        assert(f.owner->settled() && f.hal.callbacks.metadata.size() == 1);
        assert(f.hal.callbacks.metadata[0].values.at(ANDROID_CONTROL_VIDEO_STABILIZATION_MODE)[0] == 1);
        const auto &m = f.hal.callbacks.metadata[0].values;
        assert(m.at(QCAMERA3_DIAGNOSTICS_VSTAB_GYRO_COUNT)[0] == 17);
        assert(m.at(QCAMERA3_DIAGNOSTICS_VSTAB_RENDERER)[0] == 0);
        assert(m.at(QCAMERA3_DIAGNOSTICS_VSTAB_GPU_STATUS)[0] == -ENODEV);
    }
    {
        Fixture f(false, false, true);
        f.metadata();
        f.hal.mms.available = true;
        f.hal.mms.diagnostics = {0, -EIO, -EIO, 5, 0, -EIO};
        f.buffer(2, 1); f.buffer(1);
        const auto &m = f.hal.callbacks.metadata.at(0).values;
        assert(m.at(ANDROID_CONTROL_VIDEO_STABILIZATION_MODE)[0] == 0);
        assert(m.at(QCAMERA3_DIAGNOSTICS_VSTAB_REASON)[0] == -EIO);
        assert(m.at(QCAMERA3_DIAGNOSTICS_VSTAB_CORE_STATUS)[0] == -EIO);
        assert(m.at(QCAMERA3_DIAGNOSTICS_VSTAB_GYRO_COUNT)[0] == 5);
        assert(m.at(QCAMERA3_DIAGNOSTICS_VSTAB_RENDERER)[0] == 0);
        assert(m.at(QCAMERA3_DIAGNOSTICS_VSTAB_GPU_STATUS)[0] == -EIO);
        assert(f.owner->settled());
    }
    {
        Fixture f(false, false, true);
        f.metadata();
        f.hal.mms.available = true;
        f.hal.mms.frameNumber = 100; // The previous frame cannot prove this frame stabilized.
        f.hal.mms.diagnostics = {2, 0, 0};
        f.buffer(2);
        const auto &m = f.hal.callbacks.metadata.at(0).values;
        assert(m.at(ANDROID_CONTROL_VIDEO_STABILIZATION_MODE)[0] == 0);
        assert(m.count(QCAMERA3_DIAGNOSTICS_VSTAB_PATH) == 0);
        assert(m.count(QCAMERA3_DIAGNOSTICS_VSTAB_GYRO_COUNT) == 0);
        assert(m.count(QCAMERA3_DIAGNOSTICS_VSTAB_RENDERER) == 0);
        assert(m.count(QCAMERA3_DIAGNOSTICS_VSTAB_GPU_STATUS) == 0);
    }
    {
        Fixture f(false, false, true, true);
        f.metadata(); f.buffer(0); f.buffer(1);
        assert(f.hal.callbacks.metadata.empty()); // A JPEG cannot complete a selected MMS video target.
        f.hal.mms.available = true;
        f.hal.mms.diagnostics = {2, 0, 0};
        f.buffer(2);
        assert(f.owner->settled() && f.hal.callbacks.metadata.size() == 1);
        assert(f.hal.callbacks.metadata[0].values.at(ANDROID_CONTROL_VIDEO_STABILIZATION_MODE)[0] == 1);
    }
    {
        Fixture f(true, false, false, true);
        f.metadata(); f.buffer(2); f.buffer(1);
        assert(f.hal.callbacks.metadata.empty()); // A video cannot complete a selected native JPEG target.
        f.buffer(0);
        assert(f.owner->settled() && f.hal.callbacks.metadata.size() == 1);
    }
    {
        Fixture f(false, false, false, false, true);
        f.metadata(); f.buffer(1);
        assert(f.hal.callbacks.metadata.empty());
        f.hal.hasStages = true; f.hal.hasTrace = false;
        f.hal.stageId = 30; f.hal.status = 0; f.hal.applied = 1;
        f.buffer(2);
        const auto &m = f.hal.callbacks.metadata.at(0).values;
        assert((m.at(QCAMERA3_DIAGNOSTICS_STAGE_RECORDS) == std::vector<int32_t>{30, 0, 1, 101}));
        assert(m.count(QCAMERA3_NATIVE_ALGORITHM_APPLIED_NATIVE_MODE) == 0);
        assert(f.owner->settled());
    }
    {
        Fixture f(false, false, false, false, true);
        f.buffer(2); f.buffer(1);
        assert(f.hal.callbacks.metadata.empty());
        f.hal.hasStages = true; f.hal.hasTrace = false;
        f.hal.stageId = 10; f.hal.status = -ENODATA; f.hal.applied = 0;
        f.metadata();
        const auto &m = f.hal.callbacks.metadata.at(0).values;
        assert((m.at(QCAMERA3_DIAGNOSTICS_STAGE_RECORDS) == std::vector<int32_t>{10, -ENODATA, 0, 101}));
        assert(m.count(QCAMERA3_NATIVE_ALGORITHM_APPLIED_NATIVE_MODE) == 0);
    }
    {
        Fixture f(false);
        f.owner->previewBokehOutput = &f.preview;
        f.hal.hasTrace = false;
        f.metadata(); f.buffer(0);
        assert(f.hal.callbacks.metadata.empty());
        f.hal.hasStages = true; f.hal.stageId = 23; f.hal.status = 0; f.hal.applied = 1;
        f.buffer(1);
        const auto &m = f.hal.callbacks.metadata.at(0).values;
        assert((m.at(QCAMERA3_DIAGNOSTICS_STAGE_RECORDS) == std::vector<int32_t>{23, 0, 1, 101}));
        assert(m.count(QCAMERA3_NATIVE_ALGORITHM_APPLIED_NATIVE_MODE) == 0);
    }
    for (bool previewFirst : {false, true}) {
        Fixture f(true, false, true, true);
        f.owner->previewBokehOutput = &f.preview;
        f.metadata(); f.buffer(0);
        f.hal.mms.available = true;
        f.hal.mms.diagnostics = {2, 0, 0};
        f.hal.hasStages = true; f.hal.hasTrace = false;
        f.hal.stageId = 23; f.hal.status = -EIO; f.hal.applied = 0;
        f.buffer(previewFirst ? 1 : 2);
        assert(f.hal.callbacks.metadata.empty());
        f.buffer(previewFirst ? 2 : 1);
        const auto &m = f.hal.callbacks.metadata.at(0).values;
        assert(m.at(ANDROID_CONTROL_VIDEO_STABILIZATION_MODE)[0] == 1);
        assert((m.at(QCAMERA3_DIAGNOSTICS_STAGE_RECORDS) == std::vector<int32_t>{23, -EIO, 0, 101}));
        assert(f.owner->settled());
    }
    {
        Fixture f(false);
        f.owner->previewBokehOutput = &f.preview;
        f.hal.hasStages = true; f.hal.hasTrace = false;
        f.hal.stageId = 23; f.hal.status = -ENODATA; f.hal.applied = 0;
        f.buffer(1); f.buffer(0); f.metadata();
        assert(f.hal.callbacks.metadata.size() == 1);
        assert(f.hal.callbacks.metadata[0].values.at(QCAMERA3_DIAGNOSTICS_STAGE_RECORDS)[2] == 0);
    }
    std::puts("deferred result tests: all checks passed");
}
