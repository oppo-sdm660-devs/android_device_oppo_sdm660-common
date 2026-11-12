/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef FAKE_PANORAMA_GYRO_SOURCE_H
#define FAKE_PANORAMA_GYRO_SOURCE_H

#include <stdint.h>

namespace qcamera {
void panoramaTestGyroReset(int32_t startStatus = 0);
int32_t panoramaTestGyroEmit(int64_t timestampNs, const float angularVelocity[3]);
int panoramaTestGyroStarts();
int panoramaTestGyroStops();
int64_t panoramaTestGyroPeriod();
}
#endif
