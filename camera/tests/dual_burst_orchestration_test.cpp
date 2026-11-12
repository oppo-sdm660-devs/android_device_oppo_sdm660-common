/* SPDX-License-Identifier: BSD-3-Clause */
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>
#include <array>
#include <pthread.h>

constexpr int NO_ERROR = 0, BAD_VALUE = -EINVAL, HAL_PIXEL_FORMAT_BLOB = 33;
enum : uint32_t {
    QCAMERA3_NATIVE_ALGORITHM_BEAUTY_STRENGTH = 1,
    ANDROID_STATISTICS_INFO_AVAILABLE_FACE_DETECT_MODES,
    ANDROID_STATISTICS_FACE_DETECT_MODE, ANDROID_CONTROL_AE_LOCK, ANDROID_CONTROL_AWB_LOCK,
    ANDROID_CONTROL_ENABLE_ZSL, ANDROID_CONTROL_AF_TRIGGER, ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER,
};
constexpr uint8_t ANDROID_STATISTICS_FACE_DETECT_MODE_OFF = 0;
constexpr uint8_t ANDROID_STATISTICS_FACE_DETECT_MODE_SIMPLE = 1;
constexpr uint8_t ANDROID_STATISTICS_FACE_DETECT_MODE_FULL = 2;
struct Value { size_t count; std::vector<uint8_t> bytes; };
struct camera_metadata_t { std::map<uint32_t, Value> values; };
struct camera_metadata_ro_entry_t {
    size_t count = 0;
    union { const uint8_t *u8; const int32_t *i32; } data = {};
};
static int find_camera_metadata_ro_entry(const camera_metadata_t *metadata, uint32_t tag,
        camera_metadata_ro_entry_t *entry) {
    if (!metadata || !metadata->values.count(tag)) { *entry = {}; return -ENOENT; }
    const auto &value = metadata->values.at(tag);
    entry->count = value.count; entry->data.u8 = value.bytes.data(); return 0;
}
static int liveMetadataLocks = 0;
class CameraMetadata {
public:
    camera_metadata_t data;
    bool locked = false;
    CameraMetadata() = default;
    CameraMetadata(const CameraMetadata &other) : data(other.data) {}
    CameraMetadata &operator=(const CameraMetadata &other) { assert(!locked); data = other.data; return *this; }
    CameraMetadata &operator=(const camera_metadata_t *other) { assert(!locked); data = *other; return *this; }
    ~CameraMetadata() { assert(!locked); }
    bool isEmpty() const { return data.values.empty(); }
    camera_metadata_ro_entry_t find(uint32_t tag) const {
        camera_metadata_ro_entry_t entry; find_camera_metadata_ro_entry(&data, tag, &entry); return entry;
    }
    template<class T> void update(uint32_t tag, const T *items, size_t count) {
        assert(!locked);
        auto &value = data.values[tag]; value.count = count; value.bytes.resize(sizeof(T) * count);
        memcpy(value.bytes.data(), items, value.bytes.size());
    }
    const camera_metadata_t *getAndLock() { assert(!locked); locked = true; ++liveMetadataLocks; return &data; }
    void unlock(const camera_metadata_t *pointer) { assert(locked && pointer == &data); locked = false; --liveMetadataLocks; }
};
template<class T> using List = std::vector<T>;
struct camera3_stream_t { int format; };
struct camera3_stream_buffer_t { camera3_stream_t *stream; };
struct camera3_capture_request_t {
    uint32_t frame_number;
    const camera_metadata_t *settings;
    uint32_t num_output_buffers;
    const camera3_stream_buffer_t *output_buffers;
    const camera3_stream_buffer_t *input_buffer = nullptr;
};
struct InternalRequest { camera3_stream_t *stream = nullptr; bool need_metadata = false; };
struct QCamera3StillAlgo { static bool available; static bool isAvailable() { return available; } };
bool QCamera3StillAlgo::available = true;
static camera_metadata_t *gStaticMetadata[1];
struct FrameDb {
    uint32_t next = 100;
    std::map<uint32_t, uint32_t> framework;
    void generateStoreInternalFrameNumber(uint32_t &id) { id = next++; }
    void allocStoreInternalFrameNumber(uint32_t request, uint32_t &id) { id = next++; framework[id] = request; }
};
struct FakeDualCapture {
    uint64_t active = 0;
    uint32_t frame = UINT32_MAX;
    int cancellations = 0;
    uint64_t tokenFor(uint32_t camera, uint32_t id) { return (camera == 11 || camera == 22) && id == frame ? active : 0; }
    bool cancel(uint64_t token) { assert(token == active); active = 0; ++cancellations; return true; }
};
struct FakePicChannel {
    FakeDualCapture dual;
    int waits = 0, waitFailureAt = -1, cancelAtWait = -1, cancelledGroups = 0;
    uint32_t cancelledFrame = UINT32_MAX;
    int32_t waitForSnapshotBuffer(std::atomic<bool> *cancelled) {
        const int current = waits++;
        if (current == cancelAtWait) cancelled->store(true);
        return current == waitFailureAt ? -ETIMEDOUT : 0;
    }
    void cancelHdrCapture(uint32_t id) { ++cancelledGroups; cancelledFrame = id; }
    uint32_t getMyCamHandle() { return 11; }
    FakeDualCapture &getDualCapture() { return dual; }
};
class QCamera3HardwareInterface {
public:
    struct Submission { camera3_capture_request_t request; CameraMetadata settings; List<InternalRequest> internal; };
    FakePicChannel picture;
    FakePicChannel *mPictureChannel = &picture;
    CameraMetadata mLastFrameworkSettings;
    uint32_t mCameraId = 0, mSubmittingBurstFrameNumber = UINT32_MAX;
    uint32_t mDualBurstFrameIds[3] = {};
    bool mDualBurstPending = false;
    pthread_mutex_t mMutex = PTHREAD_MUTEX_INITIALIZER;
    bool configurationReady = false;
    std::atomic<bool> mAsyncCancelled{false};
    bool mRestoreFrameworkSettings = false;
    FrameDb _orchestrationDb;
    int prepareStatus = 0, prepareCalls = 0, submitFailureAt = -1, cancelAfterSubmission = -1;
    uint32_t deferred = UINT32_MAX, preparedBase = UINT32_MAX;
    std::vector<uint32_t> preparedFrames;
    std::vector<Submission> submissions;
    ~QCamera3HardwareInterface() { assert(pthread_mutex_destroy(&mMutex) == 0); }
    int32_t beginVerifiedDualRequest(uint32_t base, const CameraMetadata &, const uint32_t *frames,
            unsigned count) {
        ++prepareCalls;
        assert(configurationReady && submissions.empty());
        if (prepareStatus) return prepareStatus;
        preparedBase = base; preparedFrames.assign(frames, frames + count);
        picture.dual.active = 7; picture.dual.frame = base;
        return 0;
    }
    void deferAsyncFinalMetadata(uint32_t id) { deferred = id; }
    int32_t processCaptureRequest(camera3_capture_request_t *request, List<InternalRequest> &internal) {
        // The production binding block runs after STREAM_INFO/sensor readback,
        // before this boundary performs its first queue operation.
        configurationReady = true;
        CameraMetadata meta; meta = request->settings;
        int32_t rc = NO_ERROR;
        pthread_mutex_lock(&mMutex);
#include "dual_prepare.inc"
        pthread_mutex_unlock(&mMutex);
        Submission submission;
        submission.request = *request; submission.settings = request->settings; submission.internal = internal;
        submissions.push_back(submission);
        const int current = int(submissions.size()) - 1;
        if (current == cancelAfterSubmission) mAsyncCancelled = true;
        return current == submitFailureAt ? -EIO : 0;
    }
    int32_t orchestrateDualCapture(camera3_capture_request_t *request);
};

#include "dual_burst.inc"

static uint8_t byte(const CameraMetadata &metadata, uint32_t tag) {
    const auto entry = metadata.find(tag); assert(entry.count == 1); return entry.data.u8[0];
}
int main() {
    CameraMetadata capabilities;
    const uint8_t faces[] = {0, 1, 2};
    capabilities.update(ANDROID_STATISTICS_INFO_AVAILABLE_FACE_DETECT_MODES, faces, 3);
    gStaticMetadata[0] = &capabilities.data;
    CameraMetadata original;
    const int32_t strength = 70;
    const uint8_t unlocked = 0, zsl = 1, trigger = 1;
    original.update(QCAMERA3_NATIVE_ALGORITHM_BEAUTY_STRENGTH, &strength, 1);
    original.update(ANDROID_CONTROL_AE_LOCK, &unlocked, 1);
    original.update(ANDROID_CONTROL_AWB_LOCK, &unlocked, 1);
    original.update(ANDROID_CONTROL_ENABLE_ZSL, &zsl, 1);
    original.update(ANDROID_CONTROL_AF_TRIGGER, &trigger, 1);
    original.update(ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER, &trigger, 1);
    camera3_stream_t jpeg{HAL_PIXEL_FORMAT_BLOB}, preview{34};
    const camera3_stream_buffer_t outputs[] = {{&preview}, {&jpeg}};
    camera3_capture_request_t request{55, &original.data, 2, outputs};
    {
        QCamera3HardwareInterface hw;
        assert(hw.orchestrateDualCapture(&request) == 0);
        assert(hw.submissions.size() == 3 && hw.preparedFrames.size() == 3 && hw.prepareCalls == 1);
        assert(hw.preparedBase == hw.submissions[2].request.frame_number && hw.preparedFrames[0] == hw.preparedBase);
        assert(hw.preparedFrames[1] == hw.submissions[0].request.frame_number &&
                hw.preparedFrames[2] == hw.submissions[1].request.frame_number);
        assert(hw._orchestrationDb.framework.size() == 1 && hw._orchestrationDb.framework.at(hw.preparedBase) == 55);
        for (unsigned i = 0; i < 3; ++i) {
            const auto &submitted = hw.submissions[i];
            assert(hw.picture.dual.tokenFor(22, submitted.request.frame_number) == (i == 2 ? 7U : 0U));
            assert(submitted.request.num_output_buffers == (i == 2 ? 2U : 0U));
            assert(submitted.request.output_buffers == outputs);
            assert(submitted.internal.size() == (i == 2 ? 0U : 1U));
            if (i != 2) assert(submitted.internal[0].stream == &jpeg && submitted.internal[0].need_metadata);
            assert(byte(submitted.settings, ANDROID_CONTROL_AE_LOCK) == 1);
            assert(byte(submitted.settings, ANDROID_CONTROL_AWB_LOCK) == 1);
            assert(byte(submitted.settings, ANDROID_CONTROL_ENABLE_ZSL) == 0);
            assert(byte(submitted.settings, ANDROID_STATISTICS_FACE_DETECT_MODE) == 2);
            assert(byte(submitted.settings, ANDROID_CONTROL_AF_TRIGGER) == (i == 2 ? 1 : 0));
            assert(byte(submitted.settings, ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER) == (i == 2 ? 1 : 0));
        }
        assert(hw.deferred == 55 && hw.picture.waits == 3 && hw.picture.cancelledGroups == 0);
        assert(hw.mSubmittingBurstFrameNumber == UINT32_MAX && !hw.mDualBurstPending && liveMetadataLocks == 0);
    }
    {
        QCamera3HardwareInterface hw; hw.prepareStatus = -ESTALE;
        assert(hw.orchestrateDualCapture(&request) == -ESTALE && hw.submissions.empty());
        assert(hw.deferred == 55 && hw.mSubmittingBurstFrameNumber == UINT32_MAX && !hw.mDualBurstPending);
    }
    for (int failure = 0; failure < 3; ++failure) {
        QCamera3HardwareInterface hw; hw.picture.waitFailureAt = failure;
        assert(hw.orchestrateDualCapture(&request) == -ETIMEDOUT);
        assert(hw.submissions.size() == unsigned(failure) && hw.picture.cancelledGroups == 1);
        assert(hw.picture.dual.cancellations == (failure == 0 ? 0 : 1) && hw.picture.dual.active == 0);
        assert(hw.picture.cancelledFrame == hw.mDualBurstFrameIds[0] && hw.mRestoreFrameworkSettings);
        assert(hw.mSubmittingBurstFrameNumber == UINT32_MAX && !hw.mDualBurstPending && liveMetadataLocks == 0);
    }
    for (int failure = 0; failure < 3; ++failure) {
        QCamera3HardwareInterface hw; hw.submitFailureAt = failure;
        assert(hw.orchestrateDualCapture(&request) == -EIO);
        assert(hw.submissions.size() == unsigned(failure + 1) && hw.picture.cancelledGroups == 1);
        assert(hw.picture.dual.cancellations == 1 && hw.mRestoreFrameworkSettings && liveMetadataLocks == 0);
    }
    {
        QCamera3HardwareInterface hw; hw.cancelAfterSubmission = 0;
        assert(hw.orchestrateDualCapture(&request) == -ECANCELED && hw.submissions.size() == 1);
        assert(hw.picture.dual.cancellations == 1 && hw.picture.cancelledGroups == 1 && hw.mRestoreFrameworkSettings);
    }
    {
        QCamera3HardwareInterface hw; hw.mAsyncCancelled = true;
        assert(hw.orchestrateDualCapture(&request) == -ECANCELED && hw.submissions.empty());
    }
    {
        QCamera3HardwareInterface hw; hw.picture.cancelAtWait = 0;
        assert(hw.orchestrateDualCapture(&request) == -ECANCELED && hw.submissions.empty());
        assert(hw.mRestoreFrameworkSettings && !hw.mDualBurstPending && liveMetadataLocks == 0);
    }
    {
        QCamera3HardwareInterface hw; hw.cancelAfterSubmission = 2;
        assert(hw.orchestrateDualCapture(&request) == -ECANCELED && hw.submissions.size() == 3);
        assert(hw.picture.dual.cancellations == 1 && hw.mRestoreFrameworkSettings && !hw.mDualBurstPending);
    }
    {
        QCamera3HardwareInterface hw;
        CameraMetadata off = original;
        const int32_t zero = 0; off.update(QCAMERA3_NATIVE_ALGORITHM_BEAUTY_STRENGTH, &zero, 1);
        request.settings = &off.data;
        assert(hw.orchestrateDualCapture(&request) == 0 && hw.submissions.size() == 1 && hw.prepareCalls == 0);
        assert(hw.picture.waits == 0 && hw.submissions[0].request.num_output_buffers == 2);
        assert(byte(hw.submissions[0].settings, ANDROID_CONTROL_ENABLE_ZSL) == 1);
        request.settings = &original.data;
    }
    for (const uint8_t supportedFace : {uint8_t(0), uint8_t(1)}) {
        CameraMetadata limited;
        limited.update(ANDROID_STATISTICS_INFO_AVAILABLE_FACE_DETECT_MODES, &supportedFace, 1);
        gStaticMetadata[0] = &limited.data;
        QCamera3HardwareInterface hw;
        assert(hw.orchestrateDualCapture(&request) == 0);
        for (const auto &submission : hw.submissions)
            assert(byte(submission.settings, ANDROID_STATISTICS_FACE_DETECT_MODE) == supportedFace);
    }
    gStaticMetadata[0] = &capabilities.data;
    {
        QCamera3HardwareInterface hw;
        hw.mLastFrameworkSettings = original;
        request.settings = nullptr;
        assert(hw.orchestrateDualCapture(&request) == 0 && hw.submissions.size() == 3);
        request.settings = &original.data;
    }
    {
        QCamera3HardwareInterface hw;
        QCamera3StillAlgo::available = false;
        assert(hw.orchestrateDualCapture(&request) == 0 && hw.submissions.size() == 1);
        assert(hw.picture.waits == 0 && hw.deferred == UINT32_MAX);
        QCamera3StillAlgo::available = true;
    }
    {
        QCamera3HardwareInterface hw; hw.mPictureChannel = nullptr;
        assert(hw.orchestrateDualCapture(&request) == BAD_VALUE && hw.submissions.empty());
    }
    assert(byte(original, ANDROID_CONTROL_AE_LOCK) == 0 && byte(original, ANDROID_CONTROL_AWB_LOCK) == 0);
    assert(byte(original, ANDROID_CONTROL_ENABLE_ZSL) == 1 && liveMetadataLocks == 0);
    puts("dual portrait burst production orchestration tests: PASS");
}
