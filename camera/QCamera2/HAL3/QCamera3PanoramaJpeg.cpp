/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3PanoramaJpeg.h"

#include <errno.h>
#include <cstdlib>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#include <jpeglib.h>

namespace qcamera {
namespace {

struct ErrorManager {
    jpeg_error_mgr base;
    jmp_buf jump;
    char message[JMSG_LENGTH_MAX];
};

void onError(j_common_ptr info) {
    auto *manager = reinterpret_cast<ErrorManager *>(info->err);
    (*info->err->format_message)(info, manager->message);
    longjmp(manager->jump, 1);
}

void appendExifOrientation(std::vector<uint8_t> &jpeg, int32_t orientation) {
    // Minimal APP1 with only the TIFF Orientation tag. The caller (HWI) owns
    // the rest of the EXIF fields; this encoder must not invent exposure data.
    static const uint8_t kTiffHeader[] = {'I', 'I', 0x2a, 0x00, 0x08, 0x00, 0x00, 0x00};
    std::vector<uint8_t> app1;
    app1.insert(app1.end(), {'E', 'x', 'i', 'f', 0, 0});
    app1.insert(app1.end(), std::begin(kTiffHeader), std::end(kTiffHeader));
    app1.push_back(0x01); app1.push_back(0x00); // IFD0 entry count (LE16)
    app1.push_back(0x12); app1.push_back(0x01); // tag 0x0112 Orientation (LE16)
    app1.push_back(0x03); app1.push_back(0x00); // type SHORT (LE16)
    app1.push_back(0x01); app1.push_back(0x00); // count 1 (LE32)
    app1.push_back(0x01); app1.push_back(0x00); // count 0 (LE32 high)
    app1.push_back(uint8_t(orientation & 0xff));
    app1.push_back(uint8_t((orientation >> 8) & 0xff));
    app1.push_back(0x00); app1.push_back(0x00); // value padding
    app1.push_back(0x00); app1.push_back(0x00); app1.push_back(0x00); app1.push_back(0x00);
    const size_t segmentLength = app1.size() + 2;
    if (segmentLength > 0xffff) return;
    std::vector<uint8_t> out;
    out.reserve(jpeg.size() + 4 + app1.size());
    if (jpeg.size() < 2) return;
    out.insert(out.end(), jpeg.begin(), jpeg.begin() + 2); // SOI
    out.push_back(0xff); out.push_back(0xe1);
    out.push_back(uint8_t(segmentLength >> 8));
    out.push_back(uint8_t(segmentLength & 0xff));
    out.insert(out.end(), app1.begin(), app1.end());
    out.insert(out.end(), jpeg.begin() + 2, jpeg.end());
    jpeg.swap(out);
}

} // namespace

bool QCamera3PanoramaJpeg::validConfig(const Config &config) {
    return config.width >= 2 && config.height >= 2 &&
            (config.width & 1u) == 0 && (config.height & 1u) == 0 &&
            config.width <= 16384 && config.height <= 16384 &&
            config.quality >= 1 && config.quality <= 100 &&
            exifOrientation(config.orientationDeg) != 0;
}

int32_t QCamera3PanoramaJpeg::exifOrientation(int32_t orientationDeg) {
    switch (orientationDeg) {
        case 0: return 1;
        case 90: return 6;
        case 180: return 3;
        case 270: return 8;
        default: return 0;
    }
}

int32_t QCamera3PanoramaJpeg::encode(const uint8_t *nv21, size_t length,
        const Config &config, std::vector<uint8_t> &jpeg) {
    if (nv21 == nullptr || !validConfig(config)) return E_PARAM;
    const size_t pixels = size_t(config.width) * config.height;
    if (length != pixels + pixels / 2) return E_PARAM;

    jpeg_compress_struct compressor;
    ErrorManager error;
    std::vector<uint8_t> encoded;
    unsigned char *encodedData = nullptr;
    unsigned long encodedSize = 0;
    compressor.err = jpeg_std_error(&error.base);
    error.base.error_exit = onError;
    if (setjmp(error.jump) != 0) {
        jpeg_destroy_compress(&compressor);
        free(encodedData);
        return E_ENCODE;
    }
    jpeg_create_compress(&compressor);
    jpeg_mem_dest(&compressor, &encodedData, &encodedSize);
    compressor.image_width = config.width;
    compressor.image_height = config.height;
    compressor.input_components = 3;
    compressor.in_color_space = JCS_YCbCr;
    jpeg_set_defaults(&compressor);
    jpeg_set_colorspace(&compressor, JCS_YCbCr);
    compressor.comp_info[0].h_samp_factor = 2;
    compressor.comp_info[0].v_samp_factor = 2;
    compressor.comp_info[1].h_samp_factor = 1;
    compressor.comp_info[1].v_samp_factor = 1;
    compressor.comp_info[2].h_samp_factor = 1;
    compressor.comp_info[2].v_samp_factor = 1;
    jpeg_set_quality(&compressor, int(config.quality), TRUE);
    jpeg_start_compress(&compressor, TRUE);
    const uint8_t *vu = nv21 + pixels;
    std::vector<JSAMPLE> row(size_t(config.width) * 3);
    while (compressor.next_scanline < compressor.image_height) {
        const uint32_t y = compressor.next_scanline;
        const uint8_t *vuRow = vu + size_t(y / 2) * config.width;
        for (uint32_t x = 0; x < config.width; ++x) {
            row[size_t(x) * 3 + 0] = nv21[size_t(y) * config.width + x];
            row[size_t(x) * 3 + 1] = vuRow[(x / 2) * 2 + 1]; // U
            row[size_t(x) * 3 + 2] = vuRow[(x / 2) * 2 + 0]; // V
        }
        JSAMPROW scanline = row.data();
        jpeg_write_scanlines(&compressor, &scanline, 1);
    }
    jpeg_finish_compress(&compressor);
    jpeg_destroy_compress(&compressor);
    encoded.assign(encodedData, encodedData + encodedSize);
    free(encodedData);
    if (!encoded.empty()) appendExifOrientation(encoded, exifOrientation(config.orientationDeg));
    jpeg.swap(encoded);
    return jpeg.empty() ? E_ENCODE : OK;
}

} // namespace qcamera
