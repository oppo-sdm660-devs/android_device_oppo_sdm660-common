/* SPDX-License-Identifier: BSD-3-Clause */
#include "fake_panorama_gyro_source.h"
#include "QCamera3MmsGyroSource.h"

#include <mutex>

namespace qcamera {
namespace {
std::mutex mutex;
QCamera3MmsGyroSource::Listener *active = nullptr;
int32_t startStatus = 0;
int starts = 0, stops = 0;
int64_t period = 0;
}

struct QCamera3MmsGyroSource::Impl { bool started = false; };
QCamera3MmsGyroSource::QCamera3MmsGyroSource() : mImpl(new Impl) {}
QCamera3MmsGyroSource::~QCamera3MmsGyroSource() { stop(); delete mImpl; }
int32_t QCamera3MmsGyroSource::start(Listener &listener, int64_t periodNs) {
    std::lock_guard<std::mutex> lock(mutex);
    ++starts;
    if (startStatus != 0) return startStatus;
    active = &listener;
    period = periodNs;
    mImpl->started = true;
    return 0;
}
void QCamera3MmsGyroSource::stop() {
    std::lock_guard<std::mutex> lock(mutex);
    if (mImpl->started) {
        ++stops;
        active = nullptr;
        mImpl->started = false;
    }
}
void panoramaTestGyroReset(int32_t status) {
    std::lock_guard<std::mutex> lock(mutex);
    startStatus = status;
    starts = stops = 0;
    period = 0;
}
int32_t panoramaTestGyroEmit(int64_t timestampNs, const float angularVelocity[3]) {
    std::lock_guard<std::mutex> lock(mutex);
    return active ? active->onGyro(timestampNs, angularVelocity) : -ENODEV;
}
int panoramaTestGyroStarts() { std::lock_guard<std::mutex> lock(mutex); return starts; }
int panoramaTestGyroStops() { std::lock_guard<std::mutex> lock(mutex); return stops; }
int64_t panoramaTestGyroPeriod() { std::lock_guard<std::mutex> lock(mutex); return period; }
}
