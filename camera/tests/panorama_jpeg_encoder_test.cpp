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
    if (ifd[6] != 1 || ifd[7] != 0 || ifd[8] != 0 || ifd[9] != 0) return false;
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

void testCapacityFailurePreservesCallerOutput() {
    auto nv21 = makeNv21(256, 128);
    QCamera3PanoramaJpeg::Config config;
    config.width = 256;
    config.height = 128;
    config.maxJpegBytes = 1024;
    std::vector<uint8_t> jpeg = {1, 2, 3};
    CHECK(QCamera3PanoramaJpeg::encode(nv21.data(), nv21.size(), config, jpeg) ==
            QCamera3PanoramaJpeg::E_ENCODE);
    CHECK(jpeg == std::vector<uint8_t>({1, 2, 3}));
}

uint32_t read32(const uint8_t *p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
uint32_t ifdValue(const uint8_t *tiff, uint32_t offset, uint16_t tag) {
    const uint8_t *ifd = tiff + offset;
    const unsigned count = ifd[0] + unsigned(ifd[1]) * 256;
    for (unsigned i = 0; i < count; ++i) {
        const uint8_t *item = ifd + 2 + i * 12;
        if (unsigned(item[0]) + unsigned(item[1]) * 256 == tag) {
            CHECK(read32(item + 4) == 1);
            return read32(item + 8);
        }
    }
    return 0;
}
void testSourceExif() {
    const auto nv21 = makeNv21(64, 48);
    QCamera3PanoramaJpeg::Config config;
    config.width = 64; config.height = 48;
    std::vector<uint8_t> jpeg;
    CHECK(QCamera3PanoramaJpeg::encode(nv21.data(), nv21.size(), config, jpeg) == 0);
    const std::vector<uint8_t> xmp = {0xff, 0xe1, 0, 6, 'X', 'M', 'P', 0};
    jpeg.insert(jpeg.begin() + 2, xmp.begin(), xmp.end());
    QCamera3PanoramaJpeg::Metadata metadata;
    metadata.width = 64; metadata.height = 48;
    metadata.exposureTimeNs = 125000000; metadata.sensitivity = 800;
    CHECK(QCamera3PanoramaJpeg::replaceExif(jpeg, metadata) == 0);
    CHECK(decodesTo(jpeg, 64, 48));
    const uint8_t *tiff = jpeg.data() + 12;
    CHECK(ifdValue(tiff, 8, 0x112) == 1);
    CHECK(ifdValue(tiff, 8, 0x100) == 64 && ifdValue(tiff, 8, 0x101) == 48);
    const uint32_t exif = ifdValue(tiff, 8, 0x8769);
    CHECK(ifdValue(tiff, exif, 0xa002) == 64 && ifdValue(tiff, exif, 0xa003) == 48);
    CHECK(ifdValue(tiff, exif, 0x8827) == 800);
    const uint32_t exposure = ifdValue(tiff, exif, 0x829a);
    CHECK(read32(tiff + exposure) == 1 && read32(tiff + exposure + 4) == 8);
    const size_t nextSegment = 4 + size_t(jpeg[4]) * 256 + jpeg[5];
    CHECK(memcmp(jpeg.data() + nextSegment, xmp.data(), xmp.size()) == 0);
    const auto original = jpeg;
    CHECK(QCamera3PanoramaJpeg::replaceExif(jpeg, metadata) == 0 && jpeg == original);
    metadata.maxJpegBytes = jpeg.size() - 1;
    CHECK(QCamera3PanoramaJpeg::replaceExif(jpeg, metadata) == QCamera3PanoramaJpeg::E_ENCODE);
    CHECK(jpeg == original);
    metadata.maxJpegBytes = config.maxJpegBytes;
    metadata.width = 66;
    CHECK(QCamera3PanoramaJpeg::replaceExif(jpeg, metadata) == QCamera3PanoramaJpeg::E_PARAM);
    CHECK(jpeg == original);
    metadata.width = 64; metadata.exposureTimeNs = 0; metadata.sensitivity = 0;
    CHECK(QCamera3PanoramaJpeg::replaceExif(jpeg, metadata) == 0);
    tiff = jpeg.data() + 12;
    CHECK(ifdValue(tiff, ifdValue(tiff, 8, 0x8769), 0x829a) == 0);
    CHECK(ifdValue(tiff, ifdValue(tiff, 8, 0x8769), 0x8827) == 0);
    for (const int orientation : {0, 90, 180, 270}) {
        metadata.orientationDeg = orientation;
        CHECK(QCamera3PanoramaJpeg::replaceExif(jpeg, metadata) == 0);
        CHECK(ifdValue(jpeg.data() + 12, 8, 0x112) ==
                uint32_t(QCamera3PanoramaJpeg::exifOrientation(orientation)));
        CHECK(decodesTo(jpeg, 64, 48));
    }
    const auto rotated = jpeg;
    metadata.orientationDeg = 45;
    CHECK(QCamera3PanoramaJpeg::replaceExif(jpeg, metadata) == QCamera3PanoramaJpeg::E_PARAM);
    CHECK(jpeg == rotated);
}

}  // namespace

int main() {
    testValidOrientations();
    testRejectsInvalid();
    testCapacityFailurePreservesCallerOutput();
    testSourceExif();
    if (g_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    printf("panorama JPEG encoder host tests: PASS\n");
    return 0;
}
