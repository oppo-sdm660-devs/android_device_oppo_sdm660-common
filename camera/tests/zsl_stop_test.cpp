/* SPDX-License-Identifier: BSD-3-Clause */
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <future>
#include <mutex>
#include <thread>
#include <pthread.h>
#define LOGE(...) ((void)0)
constexpr int MM_CAMERA_CMD_TYPE_STOP_ZSL = 8, FALSE = 0;
struct Semaphore { std::mutex mutex; std::condition_variable changed; unsigned count = 0; };
void cam_sem_post(Semaphore *sem) {
    std::lock_guard<std::mutex> lock(sem->mutex);
    ++sem->count; sem->changed.notify_one();
}
void cam_sem_wait(Semaphore *sem) {
    std::unique_lock<std::mutex> lock(sem->mutex);
    sem->changed.wait(lock, [&] { return sem->count != 0; }); --sem->count;
}
struct mm_camera_cmdcb_t { int cmd_type; };
struct Queue { std::mutex mutex; std::deque<mm_camera_cmdcb_t *> items; };
std::atomic<bool> failEnqueue{false};
int cam_queue_enq(Queue *queue, mm_camera_cmdcb_t *node) {
    if (failEnqueue.exchange(false)) return -1;
    std::lock_guard<std::mutex> lock(queue->mutex); queue->items.push_back(node);
    return 0;
}
struct Daemon {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, finish = false;
    int status = 0;
};
int mm_camera_stop_zsl_snapshot(Daemon *daemon) {
    std::unique_lock<std::mutex> lock(daemon->mutex);
    daemon->entered = true; daemon->changed.notify_all();
    daemon->changed.wait(lock, [&] { return daemon->finish; });
    return daemon->status;
}
struct mm_channel_t {
    struct { Queue cmd_queue; Semaphore cmd_sem, sync_sem; } cmd_thread;
    Daemon *cam_obj;
    int manualZSLSnapshot = 1, stop_zsl_snapshot_rc = 0;
    pthread_mutex_t frame_req_lock = PTHREAD_MUTEX_INITIALIZER;
    unsigned frame_req_pending = 0, frame_req_cnt = 0, cur_req_idx = 0;
    uint32_t requested_frame_id[120] = {};
    int pending_cnt = 0, pending_retro_cnt = 0;
};
#include "reset.inc"
std::atomic<bool> failAllocation{false};
void *test_malloc(size_t size) { return failAllocation.exchange(false) ? nullptr : std::malloc(size); }
#define malloc test_malloc
#include "stop.inc"
#undef malloc
void command(mm_channel_t *ch_obj, mm_camera_cmdcb_t *cmd_cb) {
#include "daemon.inc"
    assert(!ch_obj->manualZSLSnapshot);
#include "ack.inc"
}
int main() {
    Daemon daemon;
    mm_channel_t channel;
    channel.cam_obj = &daemon;
    std::atomic<bool> stopping{false};
    std::thread worker([&] {
        for (;;) {
            cam_sem_wait(&channel.cmd_thread.cmd_sem);
            if (stopping.load()) return;
            mm_camera_cmdcb_t *node;
            {
                std::lock_guard<std::mutex> lock(channel.cmd_thread.cmd_queue.mutex);
                node = channel.cmd_thread.cmd_queue.items.front();
                channel.cmd_thread.cmd_queue.items.pop_front();
            }
            command(&channel, node);
            std::free(node);
        }
    });
    std::mutex channelLock;
    for (int status : {0, -5, 0, -19}) {
        daemon.entered = daemon.finish = false;
        daemon.status = status;
        auto stop = std::async(std::launch::async, [&] {
            // mm_camera_stop_zsl_snapshot_ch holds ch_lock across the FSM operation.
            std::lock_guard<std::mutex> lock(channelLock);
            return mm_channel_stop_zsl_snapshot(&channel);
        });
        {
            std::unique_lock<std::mutex> lock(daemon.mutex);
            daemon.changed.wait(lock, [&] { return daemon.entered; });
        }
        assert(stop.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
        {
            std::lock_guard<std::mutex> lock(daemon.mutex);
            daemon.finish = true; daemon.changed.notify_all();
        }
        assert(stop.get() == status);
        assert(channel.cmd_thread.sync_sem.count == 0);
    }
    failAllocation.store(true);
    assert(mm_channel_stop_zsl_snapshot(&channel) == -1);
    failEnqueue.store(true);
    assert(mm_channel_stop_zsl_snapshot(&channel) == -1);
    assert(channel.cmd_thread.sync_sem.count == 0);
    stopping.store(true); cam_sem_post(&channel.cmd_thread.cmd_sem); worker.join();
    std::puts("ZSL stop tests: daemon completion and errors acknowledged; failed enqueue returns without waiting");
}
