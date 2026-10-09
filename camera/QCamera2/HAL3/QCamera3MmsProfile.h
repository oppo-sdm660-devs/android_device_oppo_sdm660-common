/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_MMS_PROFILE_H
#define QCAMERA3_MMS_PROFILE_H
#include <stdint.h>
#include <string.h>

namespace qcamera {
struct QCamera3MmsProfile {
    const char *path;
    uint32_t inputWidth, inputHeight, outputWidth, outputHeight;
    // These are calibration geometries, not padded allocation dimensions.
    static bool select(const char *sensor, const char *gyro, const char *vendor,
            uint32_t width, uint32_t height, QCamera3MmsProfile &profile) {
        profile = {};
        if (!sensor || !gyro || !vendor || strcmp(gyro, "BMI160 Gyroscope") ||
                strcmp(vendor, "BOSCH")) return false;
        bool samsung = !strcmp(sensor, "s5k2p7sq");
        if (!samsung && strcmp(sensor, "imx398")) return false;
        if (width == 1280 && height == 720) {
            profile = {samsung ? "/vendor/etc/vstab_configs/vstab_db_900p_video_0_s5k2p7sq.config" :
                    "/vendor/etc/vstab_configs/vstab_db_900p_video_0.config", 1600, 900, 1280, 720};
            return true;
        }
        if (width == 1920 && height == 1080) {
            profile = {samsung ? "/vendor/etc/vstab_configs/vstab_db_1350p_video_0_s5k2p7sq.config" :
                    "/vendor/etc/vstab_configs/vstab_db_1350p_video_0.config", 2400, 1350, 1920, 1080};
            return true;
        }
        return false;
    }
};
} // namespace qcamera
#endif
