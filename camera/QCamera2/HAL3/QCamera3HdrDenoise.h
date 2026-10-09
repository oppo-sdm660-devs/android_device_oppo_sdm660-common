/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_HDR_DENOISE_H
#define QCAMERA3_HDR_DENOISE_H

#include "QCamera3HdrAlgo.h"

namespace qcamera {

// Optional OPPO NCF/ASD stage after a successful three-frame HDR merge.
class QCamera3HdrDenoise {
public:
    struct Profile {
        uint32_t iso, width, height;
        uint8_t noise[4096];
    };
    static int32_t selectProfile(const void *pack, size_t length,
            uint32_t width, uint32_t height, int32_t iso, Profile &profile);
    // Owns all writable planes; failure/cancellation leaves the image unchanged.
    // The optional path is never supplied by the framework or vendor metadata.
    static int32_t process(const QCamera3HdrAlgo::Image &image, int32_t iso,
            bool sharpen, const std::atomic<bool> *cancelled = nullptr,
            const char *path = "/vendor/etc/camera/ashdr/ncf_pack.ncf");
};
}
#endif
