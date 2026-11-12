/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_HIGH_RESOLUTION_JPEG_H
#define QCAMERA3_HIGH_RESOLUTION_JPEG_H

#include "QCamera3HdrAlgo.h"
#include "QCamera3PanoramaJpeg.h"
#include <algorithm>
#include <errno.h>

namespace qcamera {
class QCamera3HighResolutionJpeg {
public:
    struct Options {
        QCamera3PanoramaJpeg::Config jpeg;
        uint32_t thumbnailWidth = 0, thumbnailHeight = 0, thumbnailQuality = 85;
    };
    static int32_t encode(const QCamera3HdrAlgo::Image &image, const Options &options,
            QCamera3PanoramaJpeg::Metadata metadata, std::vector<uint8_t> &result) {
        if (!QCamera3HdrAlgo::validImage(image) || image.width != options.jpeg.width ||
                image.height != options.jpeg.height || image.yOffset != 0 ||
                image.uvOffset != size_t(image.width) * image.height ||
                image.yStride != image.width || image.uvStride != image.width ||
                image.length != image.uvOffset * 3 / 2) return -EINVAL;
        const uint32_t tw = options.thumbnailWidth, th = options.thumbnailHeight;
        if ((tw || th) && (!tw || !th || (tw | th) & 1 || tw > image.width || th > image.height))
            return -EINVAL;
        metadata.thumbnail.clear();
        metadata.thumbnailWidth = metadata.thumbnailHeight = 0;
        if (tw && th) {
            std::vector<uint8_t> thumbnail(size_t(tw) * th * 3 / 2);
            uint32_t cropW = image.width, cropH = image.height;
            if (uint64_t(cropW) * th > uint64_t(cropH) * tw) cropW = uint64_t(cropH) * tw / th;
            else cropH = uint64_t(cropW) * th / tw;
            cropW &= ~1U; cropH &= ~1U;
            const uint32_t left = ((image.width - cropW) / 2) & ~1U;
            const uint32_t top = ((image.height - cropH) / 2) & ~1U;
            for (uint32_t y = 0; y < th; ++y)
                for (uint32_t x = 0; x < tw; ++x)
                    thumbnail[size_t(y) * tw + x] = image.data[
                        size_t(top + uint64_t(y) * cropH / th) * image.width +
                        left + uint64_t(x) * cropW / tw];
            for (uint32_t y = 0; y < th / 2; ++y)
                for (uint32_t x = 0; x < tw; x += 2) {
                    const size_t source = image.uvOffset +
                        size_t(top / 2 + uint64_t(y) * cropH / th) * image.width +
                        left + (uint32_t(uint64_t(x) * cropW / tw) & ~1U);
                    const size_t destination = size_t(tw) * th + size_t(y) * tw + x;
                    thumbnail[destination] = image.data[source];
                    thumbnail[destination + 1] = image.data[source + 1];
                }
            QCamera3PanoramaJpeg::Config config = options.jpeg;
            config.width = tw; config.height = th; config.quality = options.thumbnailQuality;
            config.maxJpegBytes = 60000;
            const int32_t rc = QCamera3PanoramaJpeg::encode(thumbnail.data(), thumbnail.size(),
                    config, metadata.thumbnail);
            if (rc != 0) return rc;
            metadata.thumbnailWidth = tw; metadata.thumbnailHeight = th;
        }
        std::vector<uint8_t> jpeg;
        int32_t rc = QCamera3PanoramaJpeg::encode(image.data, image.length, options.jpeg, jpeg);
        if (rc != 0) return rc;
        metadata.width = image.width; metadata.height = image.height;
        metadata.orientationDeg = options.jpeg.orientationDeg;
        metadata.maxJpegBytes = options.jpeg.maxJpegBytes;
        rc = QCamera3PanoramaJpeg::replaceExif(jpeg, metadata);
        if (rc == 0) result.swap(jpeg);
        return rc;
    }
};
} // namespace qcamera
#endif
