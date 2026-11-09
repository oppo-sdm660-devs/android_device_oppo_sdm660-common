/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3PanoramaJpeg.h"

#include <stdio.h>
#include <string.h>
#include <vector>

#include <jpeglib.h>

using qcamera::QCamera3PanoramaJpeg;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

namespace {

std::vector<uint8_t> makeNv21(uint32_t width, uint32_t height) {
    std::vector<uint8_t> data(size_t(width) * height * 3 / 2);
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x)
            data[size_t(y) * width + x] = uint8_t((x * 5 + y * 3) & 0xff);
    for (size_t i = size_t(width) * height; i < data.size(); i += 2) {
        data[i] = 128;     // V
        data[i + 1] = 96;  // U
    }
    return data;
}

bool isJpegWithExif(const std::vector<uint8_t> &jpeg, int orientation) {
    if (jpeg.size() < 4 + 10 || jpeg[0] != 0xff || jpeg[1] != 0xd8) return false;
    if (jpeg[2] != 0xff || jpeg[3] != 0xe1) return false;
    if (memcmp(jpeg.data() + 4 + 2, "Exif\0\0", 6) != 0) return false;
    // APP1 data starts after FF E1 LL LL; TIFF header is "II*\0", IFD at +8.
    const uint8_t *tiff = jpeg.data() + 4 + 2 + 6;
    const uint8_t *ifd = tiff + 8;
    if (ifd[0] != 1 || ifd[1] != 0) return false;
    if (ifd[2] != 0x12 || ifd[3] != 0x01 || ifd[4] != 3 || ifd[5] != 0) return false;
    return ifd[10] + (ifd[11] << 8) == orientation;
}

bool decodesTo(const std::vector<uint8_t> &jpeg, uint32_t width, uint32_t height) {
    jpeg_decompress_struct decoder;
    jpeg_error_mgr error;
    decoder.err = jpeg_std_error(&error);
    jpeg_create_decompress(&decoder);
    jpeg_mem_src(&decoder, jpeg.data(), jpeg.size());
    if (jpeg_read_header(&decoder, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&decoder);
        return false;
    }
    const bool ok = decoder.image_width == width && decoder.image_height == height;
    jpeg_destroy_decompress(&decoder);
    return ok;
}

void testValidOrientations() {
    const auto nv21 = makeNv21(64, 48);
    const int32_t degrees[] = {0, 90, 180, 270};
    for (int32_t degree : degrees) {
        QCamera3PanoramaJpeg::Config config;
        config.width = 64;
        config.height = 48;
        config.quality = 88;
        config.orientationDeg = degree;
        std::vector<uint8_t> jpeg;
        CHECK(QCamera3PanoramaJpeg::encode(nv21.data(), nv21.size(), config, jpeg) ==
                QCamera3PanoramaJpeg::OK);
        CHECK(isJpegWithExif(jpeg, QCamera3PanoramaJpeg::exifOrientation(degree)));
        CHECK(decodesTo(jpeg, 64, 48));
    }
}

void testRejectsInvalid() {
    auto nv21 = makeNv21(16, 16);
    QCamera3PanoramaJpeg::Config config;
    config.width = 16;
    config.height = 16;
    config.orientationDeg = 33;
    std::vector<uint8_t> jpeg;
    CHECK(!QCamera3PanoramaJpeg::validConfig(config));
    CHECK(QCamera3PanoramaJpeg::encode(nv21.data(), nv21.size(), config, jpeg) ==
            QCamera3PanoramaJpeg::E_PARAM);
    config.orientationDeg = 0;
    config.quality = 0;
    CHECK(!QCamera3PanoramaJpeg::validConfig(config));
    config.quality = 95;
    CHECK(QCamera3PanoramaJpeg::encode(nv21.data(), nv21.size() - 1, config, jpeg) ==
            QCamera3PanoramaJpeg::E_PARAM);
    CHECK(QCamera3PanoramaJpeg::encode(nullptr, 0, config, jpeg) ==
            QCamera3PanoramaJpeg::E_PARAM);
}

}  // namespace

int main() {
    testValidOrientations();
    testRejectsInvalid();
    if (g_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    printf("panorama JPEG encoder host tests: PASS\n");
    return 0;
}
