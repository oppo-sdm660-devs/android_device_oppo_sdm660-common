/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3MmsGyroSource.h"
#include "QCamera3MmsVstab.h"

#include <android/frameworks/sensorservice/1.0/ISensorManager.h>
#include <android/frameworks/sensorservice/1.0/IEventQueue.h>
#include <android/frameworks/sensorservice/1.0/IEventQueueCallback.h>
#include <errno.h>
#include <new>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

namespace qcamera {
namespace {
using android::frameworks::sensorservice::V1_0::ISensorManager;
using android::frameworks::sensorservice::V1_0::IEventQueue;
using android::frameworks::sensorservice::V1_0::IEventQueueCallback;
using android::frameworks::sensorservice::V1_0::Result;
using android::hardware::sensors::V1_0::Event;
using android::hardware::sensors::V1_0::SensorInfo;
using android::hardware::sensors::V1_0::SensorType;
using android::hardware::Return;
using android::hardware::Void;
using android::sp;
struct Callback : public IEventQueueCallback {
    pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    QCamera3MmsVstab *engine = nullptr;
    QCamera3MmsGyroSource::Notify notify = nullptr;
    void *user = nullptr;
    int32_t handle = -1;
    int32_t gravityHandle = -1;
    int32_t gravityType = -1;
    int64_t periodNs = 0;
    bool active = false;
    uint64_t gyroDelivered = 0, gravityDelivered = 0;
    QCamera3MmsGyroSource::Listener *listeners[QCamera3MmsGyroSource::kMaxListeners] = {};
    uint32_t listenerCount = 0;
    std::atomic<int32_t> error{0};
    ~Callback() override { pthread_mutex_destroy(&lock); }
    Return<void> onEvent(const Event &event) override {
        const float values[] = {event.u.vec3.x, event.u.vec3.y, event.u.vec3.z};
        pthread_mutex_lock(&lock);
        if (!active) {
            pthread_mutex_unlock(&lock);
            return Void();
        }
        if (event.sensorType == SensorType::GYROSCOPE && event.sensorHandle == handle) {
            int32_t status = -ENODEV;
            if (engine) status = engine->addGyro(event.timestamp, values);
            if (status) error.store(status);
            else if (notify) notify(user);
            // The fan-out is independent of the engine's verdict: every real
            // sample reaches every listener, and a listener refusal never
            // changes what the engine or the next listener sees.
            for (uint32_t i = 0; i < listenerCount; ++i) {
                if (!listeners[i]) continue;
                if (listeners[i]->onGyro(event.timestamp, values) == 0) gyroDelivered++;
            }
        } else if (gravityHandle >= 0 && event.sensorHandle == gravityHandle &&
                (event.sensorType == SensorType::GRAVITY ||
                 event.sensorType == SensorType::ACCELEROMETER)) {
            for (uint32_t i = 0; i < listenerCount; ++i) {
                if (!listeners[i]) continue;
                if (listeners[i]->onGravity(event.timestamp, values) == 0) gravityDelivered++;
            }
        }
        pthread_mutex_unlock(&lock);
        return Void();
    }
    void disconnect() {
        pthread_mutex_lock(&lock);
        active = false;
        engine = nullptr; notify = nullptr; user = nullptr;
        gravityHandle = -1; gravityType = -1;
        pthread_mutex_unlock(&lock);
    }
};
int32_t getSensor(const sp<ISensorManager> &manager, SensorInfo &info) {
    if (!manager) return -ENODEV;
    bool received = false;
    auto transport = manager->getSensorList(
            [&](const auto &sensors, Result result) {
                if (result != Result::OK) return;
                for (const SensorInfo &sensor : sensors) {
                    // AOSP corrected/fused or uncalibrated sensors are not the
                    // physical calibrated BMI160 used by these stock profiles.
                    if (sensor.type == SensorType::GYROSCOPE &&
                            !strcmp(sensor.name.c_str(), "BMI160 Gyroscope") &&
                            !strcmp(sensor.vendor.c_str(), "BOSCH")) {
                        if (received) { received = false; return; }
                        info = sensor; received = true;
                    }
                }
            });
    if (!transport.isOk() || !received || info.type != SensorType::GYROSCOPE || info.minDelay <= 0)
        return -ENODEV;
    return 0;
}
// Optional real gravity feed of the fan-out seam. The fused TYPE_GRAVITY
// sensor is preferred; a unit without one falls back to the real
// accelerometer, whose at-rest convention (points away from the Earth centre,
// magnitude ~9.81 m/s^2) is what a consumer validates. Absence is not an error:
// a gravity consumer then carries its own documented no-gravity penalty.
int32_t getGravitySensor(const sp<ISensorManager> &manager, SensorInfo &info) {
    if (!manager) return -ENODEV;
    bool foundGravity = false, foundAccel = false;
    SensorInfo gravityInfo, accelInfo;
    auto transport = manager->getSensorList(
            [&](const auto &sensors, Result result) {
                if (result != Result::OK) return;
                for (const SensorInfo &sensor : sensors) {
                    if (sensor.minDelay <= 0) continue;
                    if (sensor.type == SensorType::GRAVITY && !foundGravity) {
                        gravityInfo = sensor; foundGravity = true;
                    } else if (sensor.type == SensorType::ACCELEROMETER && !foundAccel) {
                        accelInfo = sensor; foundAccel = true;
                    }
                }
            });
    if (!transport.isOk()) return -ENODEV;
    if (foundGravity) { info = gravityInfo; return 0; }
    if (foundAccel) { info = accelInfo; return 0; }
    return -ENODEV;
}
} // namespace
struct QCamera3MmsGyroSource::Impl {
    sp<IEventQueue> queue;
    sp<Callback> callback;
    int32_t handle = -1;
};
QCamera3MmsGyroSource::QCamera3MmsGyroSource() : mImpl(new (std::nothrow) Impl) {}
QCamera3MmsGyroSource::~QCamera3MmsGyroSource() { stop(); delete mImpl; }
int32_t QCamera3MmsGyroSource::probe(Identity &identity) {
    identity = {};
    SensorInfo info;
    int32_t status = getSensor(ISensorManager::tryGetService(), info);
    if (status) return status;
    snprintf(identity.name, sizeof(identity.name), "%s", info.name.c_str());
    snprintf(identity.vendor, sizeof(identity.vendor), "%s", info.vendor.c_str());
    identity.handle = info.sensorHandle;
    identity.minDelayUs = info.minDelay; identity.maxDelayUs = info.maxDelay;
    return 0;
}
int32_t QCamera3MmsGyroSource::addListener(Listener *listener) {
    if (!mImpl || !listener) return -EINVAL;
    if (!mImpl->callback) return -ENODEV;
    pthread_mutex_lock(&mImpl->callback->lock);
    int32_t status = 0;
    for (uint32_t i = 0; i < mImpl->callback->listenerCount; ++i) {
        if (mImpl->callback->listeners[i] == listener) status = -EALREADY;
    }
    if (!status) {
        if (mImpl->callback->listenerCount >= kMaxListeners) status = -ENOSPC;
        else mImpl->callback->listeners[mImpl->callback->listenerCount++] = listener;
    }
    pthread_mutex_unlock(&mImpl->callback->lock);
    return status;
}
int32_t QCamera3MmsGyroSource::removeListener(Listener *listener) {
    if (!mImpl || !listener) return -EINVAL;
    if (!mImpl->callback) return -ENODEV;
    pthread_mutex_lock(&mImpl->callback->lock);
    int32_t status = -ENOENT;
    for (uint32_t i = 0; i < mImpl->callback->listenerCount; ++i) {
        if (mImpl->callback->listeners[i] != listener) continue;
        for (uint32_t j = i + 1; j < mImpl->callback->listenerCount; ++j)
            mImpl->callback->listeners[j - 1] = mImpl->callback->listeners[j];
        mImpl->callback->listenerCount--;
        mImpl->callback->listeners[mImpl->callback->listenerCount] = nullptr;
        status = 0;
        break;
    }
    pthread_mutex_unlock(&mImpl->callback->lock);
    return status;
}
bool QCamera3MmsGyroSource::started() const {
    if (!mImpl || !mImpl->callback) return false;
    pthread_mutex_lock(&mImpl->callback->lock);
    const bool active = mImpl->callback->active;
    pthread_mutex_unlock(&mImpl->callback->lock);
    return active;
}
int64_t QCamera3MmsGyroSource::periodNs() const {
    if (!mImpl || !mImpl->callback) return 0;
    pthread_mutex_lock(&mImpl->callback->lock);
    const int64_t period = mImpl->callback->periodNs;
    pthread_mutex_unlock(&mImpl->callback->lock);
    return period;
}
int32_t QCamera3MmsGyroSource::gravityAvailable() const {
    if (!mImpl || !mImpl->callback) return 0;
    pthread_mutex_lock(&mImpl->callback->lock);
    const int32_t enabled = mImpl->callback->active &&
            mImpl->callback->gravityHandle >= 0 ? 1 : 0;
    pthread_mutex_unlock(&mImpl->callback->lock);
    return enabled;
}
int32_t QCamera3MmsGyroSource::start(QCamera3MmsVstab &engine, Notify notify, void *user) {
    if (!mImpl) return -ENOMEM;
    // Registration belongs to the session: a stop()/start() cycle (a flush, a
    // reconfigure) must not silently detach the listeners, so the bounded table
    // is carried across the new callback object.
    Listener *saved[kMaxListeners] = {};
    uint32_t savedCount = 0;
    if (mImpl->callback) {
        pthread_mutex_lock(&mImpl->callback->lock);
        savedCount = mImpl->callback->listenerCount;
        for (uint32_t i = 0; i < savedCount && i < kMaxListeners; ++i)
            saved[i] = mImpl->callback->listeners[i];
        pthread_mutex_unlock(&mImpl->callback->lock);
    }
    stop();
    sp<ISensorManager> manager = ISensorManager::tryGetService();
    SensorInfo info;
    int32_t status = getSensor(manager, info);
    if (status) return status;
    int64_t periodNs = engine.gyroPeriodNs();
    if (periodNs < 1000000 || periodNs > 20000000 || periodNs % 1000 ||
            periodNs / 1000 < info.minDelay ||
            (info.maxDelay > 0 && periodNs / 1000 > info.maxDelay)) return -EINVAL;
    mImpl->callback = new Callback;
    mImpl->callback->engine = &engine;
    mImpl->callback->notify = notify;
    mImpl->callback->user = user;
    mImpl->callback->handle = info.sensorHandle;
    mImpl->callback->periodNs = periodNs;
    mImpl->callback->active = true;
    for (uint32_t i = 0; i < savedCount && i < kMaxListeners; ++i)
        mImpl->callback->listeners[i] = saved[i];
    mImpl->callback->listenerCount = savedCount;
    auto transport = manager->createEventQueue(mImpl->callback,
            [&](const sp<IEventQueue> &queue, Result result) {
                if (result == Result::OK) mImpl->queue = queue;
            });
    if (!transport.isOk() || !mImpl->queue) { stop(); return -ENODEV; }
    mImpl->handle = info.sensorHandle;
    // sensorservice 1.0 uses microseconds for both arguments; zero batch
    // latency makes future gyro coverage available without another delay.
    Return<Result> enabled = mImpl->queue->enableSensor(info.sensorHandle, periodNs / 1000, 0);
    if (!enabled.isOk() || static_cast<Result>(enabled) != Result::OK) {
        stop(); return -EIO;
    }
    // Optional gravity feed on the same queue. A unit without a gravity or
    // accelerometer sensor keeps the gyro-only AR path; a real sensor that
    // cannot be enabled at this period is reported through gravityAvailable()
    // and never blocks the video feed.
    SensorInfo gravityInfo;
    if (getGravitySensor(manager, gravityInfo) == 0 &&
            gravityInfo.minDelay > 0 && periodNs / 1000 >= gravityInfo.minDelay &&
            (gravityInfo.maxDelay <= 0 || periodNs / 1000 <= gravityInfo.maxDelay)) {
        Return<Result> gravityEnabled = mImpl->queue->enableSensor(
                gravityInfo.sensorHandle, periodNs / 1000, 0);
        if (gravityEnabled.isOk() &&
                static_cast<Result>(gravityEnabled) == Result::OK) {
            mImpl->callback->gravityHandle = gravityInfo.sensorHandle;
            mImpl->callback->gravityType = int32_t(gravityInfo.type);
        }
    }
    return 0;
}
void QCamera3MmsGyroSource::stop() {
    if (!mImpl) return;
    int32_t gravityHandle = -1;
    if (mImpl->callback) {
        gravityHandle = mImpl->callback->gravityHandle;
        mImpl->callback->disconnect();
    }
    if (mImpl->queue && mImpl->handle >= 0) {
        auto result = mImpl->queue->disableSensor(mImpl->handle);
        // Queue release is still required on service failure.
        (void)result.isOk();
    }
    if (mImpl->queue && gravityHandle >= 0) {
        // The optional gravity sensor shares the queue and is disabled before
        // the queue is released.
        auto result = mImpl->queue->disableSensor(gravityHandle);
        (void)result.isOk();
    }
    mImpl->queue.clear(); mImpl->callback.clear(); mImpl->handle = -1;
}
int32_t QCamera3MmsGyroSource::lastError() const {
    return mImpl && mImpl->callback ? mImpl->callback->error.load() : -ENODEV;
}
} // namespace qcamera
