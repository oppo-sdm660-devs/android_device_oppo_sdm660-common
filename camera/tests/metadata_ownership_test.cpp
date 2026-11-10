#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <list>
#include <memory>
#include <cstring>
#include <vector>

#define LOGD(...) ((void)0)
#define LOGE(...) ((void)0)
#define LOGI(...) ((void)0)
#define LOGH(...) ((void)0)
#define IS_BUFFER_ERROR(flags) (((flags) & 0x40) != 0)
static constexpr int NO_ERROR = 0;
static constexpr int BAD_VALUE = -22;
static constexpr int CAM_STREAM_TYPE_METADATA = 7;
static constexpr int CAMERA3_BUFFER_STATUS_ERROR = 1;
static constexpr uint32_t EMPTY_FRAMEWORK_FRAME_NUMBER = UINT32_MAX;
template <class T> using List = std::list<T>;
struct Mutex { struct Autolock { explicit Autolock(Mutex &) {} }; };
struct metadata_buffer_t { int value; };
struct mm_camera_buf_def_t {
    void *buffer;
    int stream_type;
    uint32_t frame_idx;
    unsigned flags = 0;
    bool zero = false;
};
struct mm_camera_super_buf_t {
    uint32_t camera_handle, ch_id, num_bufs;
    mm_camera_buf_def_t *bufs[8];
};
struct QCamera3MetadataChannel {
    unsigned returned = 0;
    int bufDone(mm_camera_super_buf_t *frame) {
        assert(frame->num_bufs == 1 && frame->bufs[0]);
        auto *metadata = static_cast<metadata_buffer_t *>(frame->bufs[0]->buffer);
        assert(metadata->value == 73);
        metadata->value = -1;
        ++returned;
        return NO_ERROR;
    }
};
struct PostProcessor {
    bool synchronous = false;
    unsigned accepted = 0;
    std::vector<mm_camera_super_buf_t *> pending;
    std::function<void(mm_camera_super_buf_t *)> release;
    int processPPMetadata(mm_camera_super_buf_t *frame, uint32_t, bool) {
        ++accepted;
        if (synchronous) {
            release(frame);
            return -12;
        }
        pending.push_back(frame);
        return NO_ERROR;
    }
    bool releaseReprocMetaBuffer(uint32_t) { return true; }
    void drain() {
        for (auto *frame : pending) release(frame);
        pending.clear();
    }
};
struct QCamera3ProcessingChannel {
    Mutex mDropReprocBuffersLock;
    List<uint32_t> mReprocDropBufferList;
    PostProcessor m_postprocessor;
    QCamera3MetadataChannel *m_pMetaChannel;
    unsigned timeouts = 0;
    bool bundledMetadata = false;
    bool isZSLChannel() const { return bundledMetadata; }
    int32_t queueReprocMetadata(mm_camera_super_buf_t *, uint32_t, bool);
    int32_t metadataBufDone(mm_camera_super_buf_t *);
    uint32_t getMyCamHandle() const { return 9; }
    void timeoutFrame(uint32_t) { ++timeouts; }
};
#include "channel.inc"
struct Stream { void *priv; };
using buffer_handle_t = const void *;
struct camera3_stream_buffer_t {
    Stream *stream;
    buffer_handle_t *buffer;
    int status, acquire_fence, release_fence;
};
struct SourceMemory {
    uint32_t frameNumber = 42;
    uint32_t getFrameNumber(unsigned) const { return frameNumber; }
    void markFrameNumber(unsigned, uint32_t frame) { frameNumber = frame; }
};
struct OutputMemory {
    bool registered = true;
    buffer_handle_t handle = this;
    int getBufferIndex(uint32_t frame) const { return registered && frame == 42 ? 0 : -1; }
    void *getBufferHandle(int) { return &handle; }
    void unregisterBuffer(int) { assert(registered); registered = false; }
};
struct QCamera3HardwareInterface {
    unsigned snapshotStops = 0;
    void stopZslSnapshotMode() { ++snapshotStops; }
};
struct QCamera3PicChannel {
    SourceMemory sourceMemory;
    SourceMemory *mYuvMemory = &sourceMemory;
    OutputMemory mMemory;
    Stream stream{};
    Stream *mCamera3Stream = &stream;
    QCamera3HardwareInterface hw;
    void *mUserData = &hw;
    unsigned cancelled = 0, sourceReturned = 0, outputReturned = 0;
    std::function<void(void *, camera3_stream_buffer_t *, uint32_t, bool, void *)> mChannelCB;
    QCamera3PicChannel() {
        mChannelCB = [&](void *, camera3_stream_buffer_t *result, uint32_t frame, bool, void *) {
            assert(frame == 42 && result->buffer == &mMemory.handle);
            assert(result->status == CAMERA3_BUFFER_STATUS_ERROR);
            assert(result->acquire_fence == -1 && result->release_fence == -1);
            assert(!mMemory.registered && sourceReturned == 1);
            assert(hw.snapshotStops == 1);
            ++outputReturned;
        };
    }
    bool snapshotSampleIsZero(mm_camera_buf_def_t *buffer) { return buffer->zero; }
    void cancelFramePProc(uint32_t frame) { assert(frame == 42); ++cancelled; }
    void freeBufferForFrame(mm_camera_super_buf_t *) { ++sourceReturned; }
    int returnBufferError(uint32_t);
    void sourceCallback(mm_camera_super_buf_t *super_frame) {
        const uint8_t frameIndex = 0;
        auto *frame = static_cast<mm_camera_super_buf_t *>(malloc(sizeof(mm_camera_super_buf_t)));
        *frame = *super_frame;
#include "source_error.inc"
        free(frame);
        free(super_frame);
    }
};
#include "return_error.inc"

static void sourceErrors() {
    for (unsigned flags : {0x2000U, 0x2040U}) {
        for (bool zero : {false, true}) {
            QCamera3PicChannel channel;
            mm_camera_buf_def_t buffer{};
            buffer.flags = flags;
            buffer.zero = zero;
            auto *super = static_cast<mm_camera_super_buf_t *>(calloc(1, sizeof(mm_camera_super_buf_t)));
            super->num_bufs = 1;
            super->bufs[0] = &buffer;
            channel.sourceCallback(super);
            const bool error = zero || IS_BUFFER_ERROR(flags);
            assert(channel.cancelled == unsigned(error));
            assert(channel.sourceReturned == unsigned(error));
            assert(channel.outputReturned == unsigned(error));
            if (error) {
                assert(channel.sourceMemory.frameNumber == EMPTY_FRAMEWORK_FRAME_NUMBER);
                assert(channel.returnBufferError(42) == -1);
                assert(channel.outputReturned == 1);
                assert(channel.hw.snapshotStops == 1);
            }
        }
    }
    QCamera3PicChannel internal;
    assert(internal.returnBufferError(41) == -1);
    assert(internal.hw.snapshotStops == 0);
}
struct BufferInfo { Stream *stream; bool need_metadata; };
using pendingBufferIterator = List<BufferInfo>::iterator;
struct Request {
    uint32_t frame_number = 42;
    List<BufferInfo> buffers;
    List<BufferInfo> internalRequestList;
};

static void run(bool output, bool internal, bool synchronous, bool cancelled,
        bool synthetic, bool callerTracksTransfer) {
    QCamera3MetadataChannel daemon;
    QCamera3ProcessingChannel processing;
    processing.m_pMetaChannel = &daemon;
    processing.m_postprocessor.synchronous = synchronous;
    processing.m_postprocessor.release = [&](mm_camera_super_buf_t *frame) {
        processing.metadataBufDone(frame);
        free(frame);
    };
    if (cancelled) processing.mReprocDropBufferList.push_back(42);
    Stream stream{&processing};
    Request request;
    if (output) request.buffers.push_back({&stream, true});
    if (internal) request.internalRequestList.push_back({&stream, true});
    auto *i = &request;
    metadata_buffer_t native{73};
    auto *metadata = &native;
    mm_camera_buf_def_t buffer{metadata, CAM_STREAM_TYPE_METADATA, 81};
    auto *metadata_buf = static_cast<mm_camera_super_buf_t *>(calloc(1, sizeof(mm_camera_super_buf_t)));
    metadata_buf->camera_handle = 9;
    metadata_buf->ch_id = 17;
    metadata_buf->num_bufs = 1;
    metadata_buf->bufs[0] = &buffer;
    bool meta_freed = false, queued = false;
    bool *p_is_metabuf_queued = callerTracksTransfer ? &queued : nullptr;
    bool free_and_bufdone_meta_buf = !callerTracksTransfer;
    bool previewJpegFrame = false, dropFrame = false;
    uint32_t ringSourceFrame = 81;
    std::shared_ptr<metadata_buffer_t> ringMetadata;
    if (synthetic) ringMetadata = std::make_shared<metadata_buffer_t>(metadata_buffer_t{73});
    auto *mMetadataChannel = &daemon;
#include "selection.inc"
    // Translation must finish before a synchronous failure or worker can requeue native metadata.
    assert(resultMetadata->value == 73);
    assert(native.value == 73);
#include "handoff.inc"
#include "cleanup.inc"
    if (callerTracksTransfer && !queued) {
        daemon.bufDone(metadata_buf);
        free(metadata_buf);
    }
    processing.m_postprocessor.drain();
    assert(daemon.returned == 1);
    assert(internalPproc == (output || internal));
    assert(queued == (callerTracksTransfer && (output || internal) && !synthetic));
    assert(processing.m_postprocessor.accepted ==
            unsigned((output || internal) && !cancelled));
    assert(processing.timeouts == unsigned(synthetic && synchronous && !cancelled));
}

int main() {
    for (bool bundled : {false, true})
        for (bool metering : {false, true})
            for (bool incoming : {false, true}) {
                struct { bool meteringOnly, need_metadata; } requestedStream{metering, incoming};
                QCamera3ProcessingChannel processing;
                processing.bundledMetadata = bundled;
                auto *channel = &processing;
                unsigned streams_need_metadata = 0;
#include "producer.inc"
                assert(requestedStream.need_metadata == (!bundled && !metering));
                assert(streams_need_metadata == unsigned(!bundled && !metering));
                // The ZSL callback supplies one paired record; HWI supplies the other path.
                const unsigned pairedRecords = unsigned(bundled && !metering);
                assert(pairedRecords + streams_need_metadata == unsigned(!metering));
            }
    sourceErrors();
    for (bool output : {false, true})
        for (bool internal : {false, true})
            for (bool synchronous : {false, true})
                for (bool cancelled : {false, true})
                    for (bool callerTracksTransfer : {false, true})
                        run(output, internal, synchronous, cancelled, false, callerTracksTransfer);
    for (bool synchronous : {false, true})
        for (bool cancelled : {false, true})
            for (bool callerTracksTransfer : {false, true})
                run(true, false, synchronous, cancelled, true, callerTracksTransfer);
}
