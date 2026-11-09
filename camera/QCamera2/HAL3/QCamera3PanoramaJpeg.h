/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_PANORAMA_JPEG_H
#define QCAMERA3_PANORAMA_JPEG_H

#include <stddef.h>
#include <stdint.h>
#include <vector>

namespace qcamera {

/*
 * Self-contained JPEG encoder for the OSS panorama synthetic still path.
 *
 * The normal still path delegates JPEG to the daemon/PostProc job queue. That
 * queue is coupled to the camera stream and pending-request lifecycle; using it
 * for a stitched NV21 image would need a second in-flight state machine. The
 * panorama path instead encodes its canvas directly (libjpeg-turbo) and hands
 * the bytes to the HWI BLOB buffer. This keeps ordinary still/ZSL untouched.
 */
class QCamera3PanoramaJpeg {
public:
    struct Config {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t quality = 95;       // 1..100
        int32_t orientationDeg = 0;  // 0/90/180/270 -> EXIF orientation
    };

    enum Status : int32_t {
        OK = 0,
        E_PARAM = -1,
        E_NOMEM = -2,
        E_ENCODE = -3,
    };

    static bool validConfig(const Config &config);

    // Encodes packed NV21 (Y plane width*height, then interleaved VU for
    // width/2 x height/2). Appends/replaces `jpeg` only on success.
    static int32_t encode(const uint8_t *nv21, size_t length, const Config &config,
            std::vector<uint8_t> &jpeg);

    // EXIF orientation value (TIFF Orientation tag) for the config.
    static int32_t exifOrientation(int32_t orientationDeg);
};

} // namespace qcamera
#endif
