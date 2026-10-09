/* SPDX-License-Identifier: BSD-3-Clause */
#include "MctSyncProxy.h"

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <dlfcn.h>
#include <pthread.h>

static_assert(sizeof(void *) == 4, "The pinned OPPO MCT has an ARM32 ABI");

namespace {
// R11/R11s de49a88e MCT: pipeline session, sync-buffer and HAL-version fields.
constexpr size_t kSession = 0;
constexpr size_t kSyncBuffer = 0x1af0;
constexpr size_t kHalVersion = 0x1d80;
constexpr size_t kControllerPipeline = 0x7c;
constexpr size_t kPipelineBus = 0x88;
constexpr size_t kParameterQueue = 0x1d60;
constexpr size_t kParameterLock = 0x1d70;
constexpr uint32_t kBundle = 0;
constexpr uint32_t kBundleOn = 1;
constexpr uint32_t kHal3 = 3;

using Operation = int (*)(void *);
using Destroy = void (*)(void *);
using Validator = void (*)(void *, void *, const void *, const void *);
using OperateList = void (*)(void *, Validator, void *, void *);
using QueueHead = void *(*)(void *);
using QueuePush = void (*)(void *, void *);

void *library() {
    static void *handle = dlopen("libmmcamera2_mct.so", RTLD_NOW | RTLD_LOCAL);
    return handle;
}

template<class T> T original(const char *name) {
    return library() ? reinterpret_cast<T>(dlsym(library(), name)) : nullptr;
}

struct Operations {
    Operation command = original<Operation>("mct_pipeline_process_dual_cam_cmd");
    Operation zone = original<Operation>("is_dual_zone");
    Operation exists = original<Operation>("is_supermct_exist");
    Operation create = original<Operation>("supermct_new");
    Destroy destroy = original<Destroy>("supermct_destroy");
    OperateList list = original<OperateList>("mct_list_operate_nodes_multiple_data");
    QueueHead head = original<QueueHead>("mct_queue_look_at_head");
    QueuePush push = original<QueuePush>("mct_queue_push_tail");
    void *notify = original<void *>("supermct_notify");
    // The validator is private; the pinned ELF passes it through this public
    // list operation. Preserve its Thumb bit when identifying that callback.
    Validator validator = reinterpret_cast<Validator>(
            reinterpret_cast<uintptr_t>(notify) + 0x1e4);
};

const Operations &operations() {
    static const Operations value;
    return value;
}

template<class T> T field(const void *object, size_t offset) {
    T value;
    memcpy(&value, static_cast<const unsigned char *>(object) + offset, sizeof(value));
    return value;
}

template<class T> void setField(void *object, size_t offset, T value) {
    memcpy(static_cast<unsigned char *>(object) + offset, &value, sizeof(value));
}

pthread_mutex_t *parameterLock(void *pipeline) {
    return reinterpret_cast<pthread_mutex_t *>(
            static_cast<unsigned char *>(pipeline) + kParameterLock);
}

struct Pair {
    uint32_t session[2] = {};
    void *pipeline[2] = {};
    bool bundled[2] = {};
    bool registered = false;
    bool active = false;
    uint32_t dispatchedSof[2] = {};
    bool seenSof[2] = {};
};

pthread_mutex_t pairLock = PTHREAD_MUTEX_INITIALIZER;
Pair pair;

int role(uint32_t session) {
    if (pair.session[0] == session) return 0;
    if (pair.session[1] == session) return 1;
    return -1;
}

bool samePair(uint32_t mainSession, uint32_t auxSession) {
    return pair.session[0] == mainSession && pair.session[1] == auxSession;
}

bool liveParents(const Operations &ops) {
    return pair.pipeline[0] && pair.pipeline[1] &&
            ops.exists(pair.pipeline[0]) && ops.exists(pair.pipeline[1]);
}

void wakeWithoutParameters(const Operations &ops, void *pipeline, void *message) {
    void *bus = field<void *>(pipeline, kPipelineBus);
    auto *queueLock = reinterpret_cast<pthread_mutex_t *>(
            static_cast<unsigned char *>(bus) + 0x10);
    // Stock 0x6408: transfer the same owned SOF message to the controller's
    // priority queue. That controller remains responsible for freeing it.
    setField(message, 4, uint32_t{0x89});
    pthread_mutex_lock(queueLock);
    ops.push(field<void *>(bus, 8), message);
    pthread_mutex_unlock(queueLock);
    auto *mutex = field<pthread_mutex_t *>(bus, 0x14);
    pthread_mutex_lock(mutex);
    pthread_cond_signal(field<pthread_cond_t *>(bus, 0x18));
    pthread_mutex_unlock(mutex);
}

void validateParameters(void *controller1, void *controller2,
        const void *superMct, const void *message) {
    const auto &ops = operations();
    bool warmup = false;
    void *current = nullptr;
    pthread_mutex_lock(&pairLock);
    if (pair.registered && pair.active && controller1 && controller2 && message) {
        void *pipeline1 = field<void *>(controller1, kControllerPipeline);
        void *pipeline2 = field<void *>(controller2, kControllerPipeline);
        const bool matching =
                (pipeline1 == pair.pipeline[0] && pipeline2 == pair.pipeline[1]) ||
                (pipeline1 == pair.pipeline[1] && pipeline2 == pair.pipeline[0]);
        const int index = role(field<uint32_t>(message, 0));
        const void *payload = field<void *>(message, 0xc);
        if (matching && index >= 0 && payload) {
            current = pair.pipeline[index];
            const uint32_t frame = field<uint32_t>(payload, 0);
            uint32_t target = frame;
            // A previous wakeup can still be pending on a controller thread.
            // A newly marked head must be later than every dispatched SOF.
            for (int i = 0; i < 2; ++i) {
                if (pair.seenSof[i] && pair.dispatchedSof[i] >= target)
                    target = pair.dispatchedSof[i] + 1;
            }
            pair.dispatchedSof[index] = frame;
            pair.seenSof[index] = true;

            pthread_mutex_lock(parameterLock(pair.pipeline[0]));
            pthread_mutex_lock(parameterLock(pair.pipeline[1]));
            void *head1 = ops.head(field<void *>(pair.pipeline[0], kParameterQueue));
            void *head2 = ops.head(field<void *>(pair.pipeline[1], kParameterQueue));
            if (head1 && head2 && field<uint32_t>(head1, 4) == field<uint32_t>(head2, 4) &&
                    field<uint32_t>(head1, 0x14) && field<uint32_t>(head2, 0x14) &&
                    !field<uint32_t>(head1, 0x10) && !field<uint32_t>(head2, 0x10)) {
                warmup = !pair.seenSof[0] || !pair.seenSof[1];
                if (!warmup) {
                    setField(head1, 0x10, target);
                    setField(head2, 0x10, target);
                }
            }
            pthread_mutex_unlock(parameterLock(pair.pipeline[1]));
            pthread_mutex_unlock(parameterLock(pair.pipeline[0]));
        }
    }
    pthread_mutex_unlock(&pairLock);
    // The native validator and consumer acquire these locks independently.
    // Already-marked, missing, unsynchronized and mismatched heads retain the
    // original policy; only the new matching head receives a common target.
    if (warmup) wakeWithoutParameters(ops, current, const_cast<void *>(message));
    else ops.validator(controller1, controller2, superMct, message);
}
}

extern "C" int oppo_camera_mct_register_paired_sessions(uint32_t mainSession,
        uint32_t auxSession) {
    if (!mainSession || !auxSession || mainSession > UINT16_MAX ||
            auxSession > UINT16_MAX || mainSession == auxSession) return -EINVAL;
    const auto &ops = operations();
    if (!ops.command || !ops.zone || !ops.exists || !ops.create || !ops.destroy ||
            !ops.list || !ops.head || !ops.push || !ops.notify)
        return -ENOSYS;
    pthread_mutex_lock(&pairLock);
    if (!samePair(mainSession, auxSession) &&
            (pair.registered || pair.pipeline[0] || pair.pipeline[1])) {
        pthread_mutex_unlock(&pairLock);
        return -EBUSY;
    }
    if (!samePair(mainSession, auxSession)) pair = {};
    pair.session[0] = mainSession;
    pair.session[1] = auxSession;
    pair.registered = true;
    pthread_mutex_unlock(&pairLock);
    return 0;
}

extern "C" void oppo_camera_mct_unregister_paired_sessions(uint32_t mainSession,
        uint32_t auxSession) {
    const auto &ops = operations();
    pthread_mutex_lock(&pairLock);
    if (samePair(mainSession, auxSession)) {
        pair.registered = false;
        pair.active = false;
        pair.bundled[0] = pair.bundled[1] = false;
        pair.seenSof[0] = pair.seenSof[1] = false;
        if ((!pair.pipeline[0] || !ops.exists(pair.pipeline[0])) &&
                (!pair.pipeline[1] || !ops.exists(pair.pipeline[1]))) pair = {};
    }
    pthread_mutex_unlock(&pairLock);
}

extern "C" int mct_pipeline_process_dual_cam_cmd(void *pipeline) {
    const auto &ops = operations();
    if (!ops.command) return 0;
    if (!pipeline) return ops.command(pipeline);
    const uint32_t session = field<uint32_t>(pipeline, kSession);
    const void *command = field<void *>(pipeline, kSyncBuffer);
    const bool isBundle = command && field<uint32_t>(command, 0) == kBundle;
    const bool enable = isBundle && field<uint32_t>(command, 4) == kBundleOn;
    // Stock packs the related session in place while dispatching BUNDLE.
    const uint32_t related = isBundle ? field<uint32_t>(command, 0x1c) : 0;
    const int status = ops.command(pipeline);
    if (!isBundle || !status) return status;

    pthread_mutex_lock(&pairLock);
    const int index = role(session);
    const uint32_t relatedSession = related > UINT16_MAX && (related & UINT16_MAX) == 15 ?
            related >> 16 : related;
    if (!pair.registered || index < 0 || relatedSession != pair.session[1 - index] ||
            field<uint32_t>(pipeline, kHalVersion) != kHal3) {
        pthread_mutex_unlock(&pairLock);
        return status;
    }
    pair.pipeline[index] = pipeline;
    pair.bundled[index] = enable;
    const bool wasActive = pair.active;
    pair.active = false;
    if (!enable) pair.seenSof[0] = pair.seenSof[1] = false;
    if (pair.bundled[0] && pair.bundled[1]) {
        // Wait for both successful ONs: a failed peer must not leave one child
        // queued in a SuperMCT that has no processing thread yet.
        for (void *item : pair.pipeline) {
            if (!ops.exists(item) && !ops.create(item)) {
                pthread_mutex_unlock(&pairLock);
                return 0;
            }
        }
        pair.active = liveParents(ops);
        if (!pair.active) {
            pthread_mutex_unlock(&pairLock);
            return 0;
        }
        if (!wasActive) pair.seenSof[0] = pair.seenSof[1] = false;
    }
    pthread_mutex_unlock(&pairLock);
    return status;
}

extern "C" void mct_list_operate_nodes_multiple_data(void *list,
        Validator callback, void *userData1, void *userData2) {
    const auto &ops = operations();
    if (!ops.list) return;
    Validator selected = callback;
    if (callback == ops.validator) {
        pthread_mutex_lock(&pairLock);
        if (pair.registered && pair.active) selected = validateParameters;
        pthread_mutex_unlock(&pairLock);
    }
    ops.list(list, selected, userData1, userData2);
}

extern "C" int is_dual_zone(void *pipeline) {
    const auto &ops = operations();
    if (!ops.zone) return 0;
    pthread_mutex_lock(&pairLock);
    const bool managed = pipeline &&
            (pipeline == pair.pipeline[0] || pipeline == pair.pipeline[1]);
    if (managed && (!pair.registered || !pair.active || !liveParents(ops))) {
        pthread_mutex_unlock(&pairLock);
        return 0;
    }
    const int status = ops.zone(pipeline);
    pthread_mutex_unlock(&pairLock);
    return status;
}

extern "C" void supermct_destroy(void *pipeline) {
    const auto &ops = operations();
    pthread_mutex_lock(&pairLock);
    if (pipeline && (pipeline == pair.pipeline[0] || pipeline == pair.pipeline[1]))
        pair = {};
    pthread_mutex_unlock(&pairLock);
    // The existing controller close path owns destruction; do not enqueue a
    // second CLOSE_CAM from BUNDLE_OFF or application unregistration.
    if (ops.destroy) ops.destroy(pipeline);
}
