/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3HighResolutionJpeg.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <jpeglib.h>

using namespace qcamera;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #c); ++failures; } } while (0)
static uint32_t le32(const uint8_t *p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
static uint32_t tag(const uint8_t *tiff, uint32_t offset, uint16_t key) {
    const uint8_t *p = tiff + offset;
    const unsigned count = p[0] | unsigned(p[1]) << 8;
    for (unsigned i = 0; i < count; ++i) {
        const uint8_t *field = p + 2 + 12 * i;
        if ((field[0] | unsigned(field[1]) << 8) == key) return le32(field + 8);
    }
    return 0;
}
static const uint8_t *value(const uint8_t *tiff, uint32_t offset, uint16_t key) {
    const uint8_t *p = tiff + offset;
    const unsigned count = p[0] | unsigned(p[1]) << 8;
    for (unsigned i = 0; i < count; ++i) {
        const uint8_t *field = p + 2 + 12 * i;
        if ((field[0] | unsigned(field[1]) << 8) != key) continue;
        const unsigned type = field[2] | unsigned(field[3]) << 8;
        const size_t bytes = le32(field + 4) * (type == 3 ? 2 : type == 4 ? 4 : type == 5 ? 8 : 1);
        return bytes > 4 ? tiff + le32(field + 8) : field + 8;
    }
    return nullptr;
}
static uint32_t nextIfd(const uint8_t *tiff, uint32_t offset) {
    const unsigned count = tiff[offset] | unsigned(tiff[offset + 1]) << 8;
    return le32(tiff + offset + 2 + count * 12);
}
static int jpegDimensions(const uint8_t *bytes, size_t length, uint32_t width, uint32_t height,
        bool colors = false) {
    jpeg_decompress_struct decoder = {};
    jpeg_error_mgr error;
    decoder.err = jpeg_std_error(&error);
    jpeg_create_decompress(&decoder);
    jpeg_mem_src(&decoder, bytes, length);
    CHECK(jpeg_read_header(&decoder, TRUE) == JPEG_HEADER_OK);
    CHECK(decoder.image_width == width && decoder.image_height == height);
    const int quantizer = decoder.quant_tbl_ptrs[0]->quantval[0];
    if (colors) {
        decoder.out_color_space = JCS_YCbCr;
        jpeg_start_decompress(&decoder);
        std::vector<uint8_t> row(width * 3);
        while (decoder.output_scanline < decoder.output_height) {
            JSAMPROW pointer = row.data();
            jpeg_read_scanlines(&decoder, &pointer, 1);
            CHECK(std::abs(int(row[1]) - 80) <= 3 && std::abs(int(row[2]) - 200) <= 3);
        }
        jpeg_finish_decompress(&decoder);
    }
    jpeg_destroy_decompress(&decoder);
    return quantizer;
}
static QCamera3HdrAlgo::Image makeImage(std::vector<uint8_t> &bytes, uint32_t width, uint32_t height) {
    const size_t y = size_t(width) * height;
    bytes.assign(y * 3 / 2, 100);
    for (size_t n = y; n < bytes.size(); n += 2) { bytes[n] = 200; bytes[n + 1] = 80; }
    return {bytes.data(), bytes.size(), width, height, 0, y, width, width, height, height / 2};
}
int main() {
    std::vector<uint8_t> pixels, encoded;
    const auto image = makeImage(pixels, 128, 96);
    QCamera3HighResolutionJpeg::Options options;
    options.jpeg.width = image.width; options.jpeg.height = image.height;
    options.jpeg.quality = 95; options.jpeg.orientationDeg = 90;
    options.thumbnailWidth = 32; options.thumbnailHeight = 24; options.thumbnailQuality = 80;
    QCamera3PanoramaJpeg::Metadata metadata;
    metadata.exposureTimeNs = 20000000; metadata.sensitivity = 125;
    CHECK(QCamera3HighResolutionJpeg::encode(image, options, metadata, encoded) == 0);
    const int fine = jpegDimensions(encoded.data(), encoded.size(), 128, 96, true);
    const uint8_t *tiff = encoded.data() + 12;
    CHECK(tag(tiff, 8, 0x112) == 6);
    CHECK(tag(tiff, 8, 0x100) == 128 && tag(tiff, 8, 0x101) == 96);
    const uint32_t exif = tag(tiff, 8, 0x8769);
    CHECK(tag(tiff, exif, 0x8827) == 125);
    const uint32_t exposure = tag(tiff, exif, 0x829a);
    CHECK(le32(tiff + exposure) == 1 && le32(tiff + exposure + 4) == 50);
    const uint32_t thumbnail = nextIfd(tiff, 8);
    CHECK(thumbnail && tag(tiff, thumbnail, 0x112) == 6);
    CHECK(tag(tiff, thumbnail, 0x100) == 32 && tag(tiff, thumbnail, 0x101) == 24);
    jpegDimensions(tiff + tag(tiff, thumbnail, 0x201), tag(tiff, thumbnail, 0x202), 32, 24, true);
    metadata.make = "OPPO"; metadata.model = "R11"; metadata.software = "OCam HALv3";
    metadata.dateTime = "2026:11:12 08:09:10"; metadata.subsecTime = "321";
    metadata.focalLengthMm = 4.25f; metadata.userComment = "nativeStage24=applied";
    metadata.gpsCoordinatesValid = metadata.gpsTimestampValid = true;
    metadata.gpsCoordinates[0] = -31.25; metadata.gpsCoordinates[1] = 121.5;
    metadata.gpsCoordinates[2] = -10.5; metadata.gpsTimestamp = 0;
    metadata.gpsProcessingMethod = "GPS";
    CHECK(QCamera3HighResolutionJpeg::encode(image, options, metadata, encoded) == 0);
    tiff = encoded.data() + 12;
    CHECK(!strcmp(reinterpret_cast<const char *>(value(tiff, 8, 0x10f)), "OPPO"));
    CHECK(!strcmp(reinterpret_cast<const char *>(value(tiff, 8, 0x110)), "R11"));
    CHECK(!strcmp(reinterpret_cast<const char *>(value(tiff, 8, 0x131)), "OCam HALv3"));
    const uint32_t detail = tag(tiff, 8, 0x8769), gps = tag(tiff, 8, 0x8825);
    CHECK(!strcmp(reinterpret_cast<const char *>(value(tiff, detail, 0x9003)), metadata.dateTime.c_str()));
    CHECK(!strcmp(reinterpret_cast<const char *>(value(tiff, detail, 0x9291)), "321"));
    CHECK(le32(value(tiff, detail, 0x920a)) == 4250 && le32(value(tiff, detail, 0x920a) + 4) == 1000);
    CHECK(!memcmp(value(tiff, detail, 0x9286), "ASCII\0\0\0nativeStage24=applied", 29));
    CHECK(value(tiff, gps, 1)[0] == 'S' && value(tiff, gps, 3)[0] == 'E');
    CHECK(le32(value(tiff, gps, 2)) == 31 && le32(value(tiff, gps, 2) + 8) == 15);
    CHECK(le32(value(tiff, gps, 4)) == 121 && le32(value(tiff, gps, 4) + 8) == 30);
    CHECK(value(tiff, gps, 5)[0] == 1 && le32(value(tiff, gps, 6)) == 10500);
    CHECK(!strcmp(reinterpret_cast<const char *>(value(tiff, gps, 0x1d)), "1970:01:01"));
    CHECK(!memcmp(value(tiff, gps, 0x1b), "ASCII\0\0\0GPS", 11));
    CHECK(nextIfd(tiff, 8) && tag(tiff, nextIfd(tiff, 8), 0x100) == 32);
    options.jpeg.quality = 25;
    CHECK(QCamera3HighResolutionJpeg::encode(image, options, metadata, encoded) == 0);
    CHECK(jpegDimensions(encoded.data(), encoded.size(), 128, 96) > fine);
    const auto original = encoded;
    options.thumbnailWidth = 33;
    CHECK(QCamera3HighResolutionJpeg::encode(image, options, metadata, encoded) == -EINVAL);
    CHECK(encoded == original);
    options.thumbnailWidth = 32; options.jpeg.orientationDeg = 45;
    CHECK(QCamera3HighResolutionJpeg::encode(image, options, metadata, encoded) != 0);
    CHECK(encoded == original);
    options.jpeg.orientationDeg = 0; options.jpeg.width = 64;
    CHECK(QCamera3HighResolutionJpeg::encode(image, options, metadata, encoded) == -EINVAL);
    CHECK(encoded == original);
    options = {};
    options.jpeg.width = 9216; options.jpeg.height = 6912; options.jpeg.quality = 95;
    options.jpeg.orientationDeg = 270; options.jpeg.maxJpegBytes = 4 * 1024 * 1024;
    const auto large = makeImage(pixels, 9216, 6912);
    CHECK(QCamera3HighResolutionJpeg::encode(large, options, metadata, encoded) == 0);
    jpegDimensions(encoded.data(), encoded.size(), 9216, 6912);
    tiff = encoded.data() + 12;
    CHECK(tag(tiff, 8, 0x100) == 9216 && tag(tiff, 8, 0x101) == 6912 && tag(tiff, 8, 0x112) == 8);
    CHECK(nextIfd(tiff, 8) == 0);
    if (failures) return 1;
    puts("High-resolution JPEG dimensions/EXIF/thumbnail/quality tests: PASS");
}
