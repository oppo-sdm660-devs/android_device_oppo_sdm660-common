/* SPDX-License-Identifier: BSD-3-Clause */
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <pthread.h>
#include <thread>
#define LOGE(...) ((void)0)
#define LOGH(...) ((void)0)
#define LOGL(...) ((void)0)
constexpr int MM_CAMERA_CMD_TYPE_REQ_DATA_CB = 2, FALSE = 0;
struct mm_camera_req_buf_t {
    uint32_t num_buf_requested, num_retro_buf_requested, frame_idx;
    int type;
};
struct mm_camera_cmdcb_t {
    int cmd_type;
    struct { mm_camera_req_buf_t req_buf; struct { unsigned frame_idx; int stream_type; } flush_cmd; } u;
};
struct Queue { std::deque<mm_camera_cmdcb_t *> nodes; };
struct Semaphore { int count = 0; };
struct mm_channel_t {
    uint8_t cur_req_idx = 0, frame_req_cnt = 0;
    uint32_t requested_frame_id[MAX_CAPTURE_BATCH_NUM] = {};
    pthread_mutex_t frame_req_lock = PTHREAD_MUTEX_INITIALIZER;
    uint32_t frame_req_pending = 0;
    struct { pthread_mutex_t sync_lock = PTHREAD_MUTEX_INITIALIZER; int is_active = 0; } frame_sync;
    struct { Queue cmd_queue; Semaphore cmd_sem, sync_sem; } cmd_thread;
    struct { Semaphore sync_sem; } cb_thread;
    struct SuperQueue { unsigned expected_frame_id = 0; } ;
    struct { SuperQueue superbuf_queue; } bundle;
    struct Camera { int my_num = 0; } camera;
    Camera *cam_obj = &camera;
    bool match_meta = false;
    uint32_t pending_cnt = 0, pending_retro_cnt = 0;
    int req_type = 0, bWaitForPrepSnapshotDone = 0, needLEDFlash = 0;
    int stopZslSnapshot = 0, unLockAEC = 0, manualZSLSnapshot = 1, stop_zsl_snapshot_rc = 0;
};
bool failAllocation = false, failEnqueue = false;
void *test_malloc(size_t n) { return failAllocation ? nullptr : std::malloc(n); }
int cam_queue_enq(Queue *queue, mm_camera_cmdcb_t *node) {
    if (failEnqueue) return -1;
    queue->nodes.push_back(node); return 0;
}
void cam_sem_post(Semaphore *sem) { ++sem->count; }
void cam_sem_wait(Semaphore *) { assert(false && "single camera cannot wait for frame sync"); }
void mm_channel_superbuf_skip(mm_channel_t *, mm_channel_t::SuperQueue *) {}
void mm_channel_superbuf_flush(mm_channel_t *, mm_channel_t::SuperQueue *, int) {}
int mm_camera_stop_zsl_snapshot(mm_channel_t::Camera *) { return 0; }
#include "helpers.inc"
#define malloc test_malloc
#include "request.inc"
#undef malloc
void receive(mm_channel_t *ch_obj) {
    assert(!ch_obj->cmd_thread.cmd_queue.nodes.empty());
    auto *cmd_cb = ch_obj->cmd_thread.cmd_queue.nodes.front();
    ch_obj->cmd_thread.cmd_queue.nodes.pop_front();
#include "receive.inc"
    std::free(cmd_cb);
}
void consume(mm_channel_t *ch_obj) {
#include "consume.inc"
}
void stop(mm_channel_t *ch_obj) {
#include "stop.inc"
}
void flush(mm_channel_t *ch_obj) {
    auto *m_obj = ch_obj;
    mm_camera_cmdcb_t command = {}; auto *cmd_cb = &command;
#include "flush.inc"
}
void enqueue(mm_channel_t &channel, uint32_t frame) {
    mm_camera_req_buf_t request = {}; request.num_buf_requested = 1; request.frame_idx = frame;
    assert(mm_channel_request_super_buf(&channel, &request) == 0);
}
void intact(mm_channel_t &channel) {
    assert(channel.frame_sync.is_active == 0);
    assert(pthread_mutex_trylock(&channel.frame_sync.sync_lock) == 0);
    pthread_mutex_unlock(&channel.frame_sync.sync_lock);
}
int main() {
    mm_channel_t ch;
    uint32_t id = 1;
    // Real autoNight shape: 14 private source requests and the framework base.
    // No automatic ZSL stop; repeat far beyond the old 8 * 15 overwrite boundary.
    for (int round = 0; round < 80; ++round) {
        for (int i = 0; i < 15; ++i) enqueue(ch, id++);
        for (int i = 0; i < 15; ++i) receive(&ch);
        for (int i = 0; i < 15; ++i) {
            assert(ch.requested_frame_id[ch.cur_req_idx] == id - 15 + i);
            consume(&ch);
        }
        assert(ch.frame_req_cnt == 0 && ch.frame_req_pending == 0);
        if (round >= 12 && round % 3 == 0) stop(&ch);
        intact(ch);
    }
    // Bound reservations including commands not yet seen by the worker.
    for (int i = 0; i < MAX_CAPTURE_BATCH_NUM; ++i) enqueue(ch, id++);
    mm_camera_req_buf_t request = {}; request.num_buf_requested = 1; request.frame_idx = id;
    assert(mm_channel_request_super_buf(&ch, &request) == -ENOSPC);
    assert(ch.frame_req_pending == MAX_CAPTURE_BATCH_NUM);
    assert(ch.cmd_thread.cmd_queue.nodes.size() == MAX_CAPTURE_BATCH_NUM);
    for (int i = 0; i < MAX_CAPTURE_BATCH_NUM; ++i) receive(&ch);
    // Reuse consumed prefixes while requests remain, preserving FIFO order.
    for (int i = 0; i < MAX_CAPTURE_BATCH_NUM * 3; ++i) {
        uint32_t expected = ch.requested_frame_id[ch.cur_req_idx] + 1;
        consume(&ch); enqueue(ch, id++); receive(&ch);
        assert(ch.requested_frame_id[ch.cur_req_idx] == expected);
        assert(ch.frame_req_pending == MAX_CAPTURE_BATCH_NUM);
        intact(ch);
    }
    flush(&ch);
    assert(ch.cmd_thread.sync_sem.count == 1);
    assert(ch.frame_req_pending == 0 && ch.frame_req_cnt == 0);
    // CANCEL preserves reservations belonging to later queued requests.
    enqueue(ch, id++); receive(&ch);
    mm_camera_req_buf_t cancel = {};
    assert(mm_channel_request_super_buf(&ch, &cancel) == 0);
    enqueue(ch, id++); receive(&ch); receive(&ch);
    assert(ch.frame_req_pending == 1 && ch.frame_req_cnt == 1);
    consume(&ch);
    // Frame-zero preview requests neither take capacity nor overwrite indexed requests.
    enqueue(ch, 0); receive(&ch); consume(&ch);
    assert(ch.frame_req_pending == 0);
    failAllocation = true;
    assert(mm_channel_request_super_buf(&ch, &request) == -1);
    failAllocation = false; failEnqueue = true;
    assert(mm_channel_request_super_buf(&ch, &request) == -1);
    failEnqueue = false;
    assert(ch.frame_req_pending == 0 && ch.cmd_thread.cmd_queue.nodes.empty());
    intact(ch);
    std::puts("frame requests: 1200 burst frames, bounded admission, prefix reuse, stop/cancel/flush reset PASS");
}
