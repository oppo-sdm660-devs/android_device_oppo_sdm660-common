/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3PanoramaJpeg.h"

#include <errno.h>
#include <cstdlib>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#include <jpeglib.h>
#include <jerror.h>
#include <numeric>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <utility>

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

void initDestination(j_compress_ptr) {}
boolean outputFull(j_compress_ptr compressor) {
    ERREXIT(compressor, JERR_BUFFER_SIZE);
    return FALSE;
}
void finishDestination(j_compress_ptr) {}

void le16(std::vector<uint8_t> &bytes, uint16_t value) {
    bytes.push_back(uint8_t(value)); bytes.push_back(uint8_t(value >> 8));
}
void le32(std::vector<uint8_t> &bytes, uint32_t value) {
    le16(bytes, uint16_t(value)); le16(bytes, uint16_t(value >> 16));
}
struct TiffEntry {
    uint16_t tag, type;
    uint32_t count;
    std::vector<uint8_t> bytes;
};
TiffEntry number(uint16_t tag, uint16_t type, uint32_t value) {
    TiffEntry result = {tag, type, 1, {}};
    if (type == 1) result.bytes.push_back(value);
    else if (type == 3) le16(result.bytes, value);
    else le32(result.bytes, value);
    return result;
}
TiffEntry text(uint16_t tag, const std::string &value) {
    TiffEntry result = {tag, 2, uint32_t(value.size() + 1), {value.begin(), value.end()}};
    result.bytes.push_back(0);
    return result;
}
TiffEntry rational(uint16_t tag, uint32_t numerator, uint32_t denominator) {
    TiffEntry result = {tag, 5, 1, {}};
    le32(result.bytes, numerator); le32(result.bytes, denominator);
    return result;
}
void patch32(std::vector<uint8_t> &bytes, size_t position, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[position + i] = value >> (8 * i);
}
uint32_t appendIfd(std::vector<uint8_t> &bytes, std::vector<TiffEntry> fields) {
    std::sort(fields.begin(), fields.end(), [](const TiffEntry &a, const TiffEntry &b) { return a.tag < b.tag; });
    const uint32_t offset = bytes.size();
    le16(bytes, fields.size());
    bytes.resize(bytes.size() + fields.size() * 12 + 4, 0);
    for (size_t i = 0; i < fields.size(); ++i) {
        const auto &field = fields[i];
        std::vector<uint8_t> header;
        le16(header, field.tag); le16(header, field.type); le32(header, field.count);
        const size_t record = offset + 2 + 12 * i;
        memcpy(bytes.data() + record, header.data(), header.size());
        if (field.bytes.size() <= 4) memcpy(bytes.data() + record + 8, field.bytes.data(), field.bytes.size());
        else {
            patch32(bytes, record + 8, bytes.size());
            bytes.insert(bytes.end(), field.bytes.begin(), field.bytes.end());
            if (bytes.size() & 1) bytes.push_back(0);
        }
    }
    return offset;
}
void patchIfd(std::vector<uint8_t> &bytes, uint32_t ifd, uint16_t tag, uint32_t value) {
    const uint32_t count = bytes[ifd] | uint32_t(bytes[ifd + 1]) << 8;
    for (uint32_t i = 0; i < count; ++i) {
        const size_t record = ifd + 2 + 12 * i;
        if ((bytes[record] | uint32_t(bytes[record + 1]) << 8) == tag) patch32(bytes, record + 8, value);
    }
}
TiffEntry gpsCoordinate(uint16_t tag, double coordinate) {
    const uint64_t total = std::llround(std::abs(coordinate) * 3600000000.0);
    const uint32_t degrees = total / 3600000000ULL;
    const uint32_t minutes = (total % 3600000000ULL) / 60000000;
    TiffEntry result = {tag, 5, 3, {}};
    le32(result.bytes, degrees); le32(result.bytes, 1);
    le32(result.bytes, minutes); le32(result.bytes, 1);
    le32(result.bytes, total % 60000000);
    le32(result.bytes, 1000000);
    return result;
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
    app1.push_back(0x00); app1.push_back(0x00); // count (LE32 high)
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
            config.quality >= 1 && config.quality <= 100 && config.maxJpegBytes >= 1024 &&
            exifOrientation(config.orientationDeg) != 0;
}

int32_t QCamera3PanoramaJpeg::replaceExif(std::vector<uint8_t> &jpeg, const Metadata &metadata) {
    if (!metadata.width || !metadata.height || metadata.exposureTimeNs < 0 ||
            metadata.sensitivity > UINT16_MAX || !exifOrientation(metadata.orientationDeg) || jpeg.size() < 4 ||
            jpeg[0] != 0xff || jpeg[1] != 0xd8) return E_PARAM;
    if (metadata.thumbnail.empty() ? (metadata.thumbnailWidth || metadata.thumbnailHeight) :
            (!metadata.thumbnailWidth || !metadata.thumbnailHeight || metadata.thumbnail.size() < 4 ||
             metadata.thumbnail[0] != 0xff || metadata.thumbnail[1] != 0xd8)) return E_PARAM;
    uint64_t numerator = uint64_t(metadata.exposureTimeNs), denominator = 1000000000;
    const uint64_t divisor = std::gcd(numerator, denominator);
    numerator /= divisor; denominator /= divisor;
    if (numerator > UINT32_MAX) return E_PARAM;
    if (!std::isfinite(metadata.focalLengthMm) || metadata.focalLengthMm < 0 || metadata.focalLengthMm > 10000)
        return E_PARAM;
    if (metadata.gpsCoordinatesValid &&
            (!std::isfinite(metadata.gpsCoordinates[0]) || !std::isfinite(metadata.gpsCoordinates[1]) ||
             !std::isfinite(metadata.gpsCoordinates[2]) || std::abs(metadata.gpsCoordinates[0]) > 90 ||
             std::abs(metadata.gpsCoordinates[1]) > 180 || std::abs(metadata.gpsCoordinates[2]) > 4000000))
        return E_PARAM;
    std::vector<TiffEntry> main = {number(0x100, 4, metadata.width), number(0x101, 4, metadata.height),
        number(0x112, 3, exifOrientation(metadata.orientationDeg)), number(0x8769, 4, 0)};
    for (const auto &value : {std::make_pair(uint16_t(0x10f), metadata.make),
            std::make_pair(uint16_t(0x110), metadata.model), std::make_pair(uint16_t(0x131), metadata.software),
            std::make_pair(uint16_t(0x132), metadata.dateTime), std::make_pair(uint16_t(0x10e), metadata.imageDescription)})
        if (!value.second.empty()) main.push_back(text(value.first, value.second));
    const bool gps = metadata.gpsCoordinatesValid || metadata.gpsTimestampValid || !metadata.gpsProcessingMethod.empty();
    if (gps) main.push_back(number(0x8825, 4, 0));
    std::vector<TiffEntry> exif = {number(0xa002, 4, metadata.width), number(0xa003, 4, metadata.height)};
    if (metadata.exposureTimeNs) exif.push_back(rational(0x829a, numerator, denominator));
    if (metadata.sensitivity) exif.push_back(number(0x8827, 3, metadata.sensitivity));
    if (metadata.focalLengthMm) exif.push_back(rational(0x920a, std::lround(metadata.focalLengthMm * 1000), 1000));
    if (!metadata.dateTime.empty()) {
        exif.push_back(text(0x9003, metadata.dateTime)); exif.push_back(text(0x9004, metadata.dateTime));
    }
    if (!metadata.subsecTime.empty()) {
        exif.push_back(text(0x9290, metadata.subsecTime)); exif.push_back(text(0x9291, metadata.subsecTime));
        exif.push_back(text(0x9292, metadata.subsecTime));
    }
    if (!metadata.userComment.empty()) {
        TiffEntry comment = {0x9286, 7, uint32_t(metadata.userComment.size() + 8), {'A', 'S', 'C', 'I', 'I', 0, 0, 0}};
        comment.bytes.insert(comment.bytes.end(), metadata.userComment.begin(), metadata.userComment.end());
        exif.push_back(std::move(comment));
    }
    std::vector<uint8_t> tiff = {'I', 'I', 42, 0, 8, 0, 0, 0};
    appendIfd(tiff, main);
    patchIfd(tiff, 8, 0x8769, appendIfd(tiff, exif));
    if (gps) {
        std::vector<TiffEntry> fields = {{0, 1, 4, {2, 3, 0, 0}}};
        if (metadata.gpsCoordinatesValid) {
            fields.push_back(text(1, metadata.gpsCoordinates[0] < 0 ? "S" : "N"));
            fields.push_back(gpsCoordinate(2, metadata.gpsCoordinates[0]));
            fields.push_back(text(3, metadata.gpsCoordinates[1] < 0 ? "W" : "E"));
            fields.push_back(gpsCoordinate(4, metadata.gpsCoordinates[1]));
            fields.push_back(number(5, 1, metadata.gpsCoordinates[2] < 0 ? 1 : 0));
            fields.push_back(rational(6, std::llround(std::abs(metadata.gpsCoordinates[2]) * 1000), 1000));
        }
        if (metadata.gpsTimestampValid) {
            const time_t value = metadata.gpsTimestamp;
            tm utc = {};
            if (metadata.gpsTimestamp < 0 || int64_t(value) != metadata.gpsTimestamp || !gmtime_r(&value, &utc)) return E_PARAM;
            TiffEntry time = {7, 5, 3, {}};
            for (const unsigned component : {unsigned(utc.tm_hour), unsigned(utc.tm_min), unsigned(utc.tm_sec)}) {
                le32(time.bytes, component); le32(time.bytes, 1);
            }
            fields.push_back(std::move(time));
            char date[16];
            if (!strftime(date, sizeof(date), "%Y:%m:%d", &utc)) return E_PARAM;
            fields.push_back(text(0x1d, date));
        }
        if (!metadata.gpsProcessingMethod.empty()) {
            TiffEntry method = {0x1b, 7, uint32_t(metadata.gpsProcessingMethod.size() + 8),
                {'A', 'S', 'C', 'I', 'I', 0, 0, 0}};
            method.bytes.insert(method.bytes.end(), metadata.gpsProcessingMethod.begin(), metadata.gpsProcessingMethod.end());
            fields.push_back(std::move(method));
        }
        patchIfd(tiff, 8, 0x8825, appendIfd(tiff, fields));
    }
    if (!metadata.thumbnail.empty()) {
        const uint32_t thumbnailIfd = appendIfd(tiff, {number(0x100, 4, metadata.thumbnailWidth),
            number(0x101, 4, metadata.thumbnailHeight), number(0x103, 3, 6),
            number(0x112, 3, exifOrientation(metadata.orientationDeg)),
            number(0x201, 4, 0), number(0x202, 4, metadata.thumbnail.size())});
        patch32(tiff, 8 + 2 + main.size() * 12, thumbnailIfd);
        patchIfd(tiff, thumbnailIfd, 0x201, tiff.size());
        tiff.insert(tiff.end(), metadata.thumbnail.begin(), metadata.thumbnail.end());
    }
    if (tiff.size() > 65527) return E_PARAM;
    const uint16_t segmentLength = uint16_t(tiff.size() + 8);
    std::vector<uint8_t> output = {0xff, 0xd8, 0xff, 0xe1,
        uint8_t(segmentLength >> 8), uint8_t(segmentLength), 'E', 'x', 'i', 'f', 0, 0};
    output.insert(output.end(), tiff.begin(), tiff.end());
    size_t position = 2;
    bool matchingDimensions = false;
    while (position < jpeg.size()) {
        if (jpeg[position] != 0xff || position + 1 >= jpeg.size()) return E_PARAM;
        const uint8_t marker = jpeg[position + 1];
        if (marker == 0xda || marker == 0xd9) break;
        if (position + 4 > jpeg.size()) return E_PARAM;
        const size_t length = size_t(jpeg[position + 2]) * 256 + jpeg[position + 3];
        if (length < 2 || length > jpeg.size() - position - 2) return E_PARAM;
        if (marker == 0xc0 || marker == 0xc2) {
            if (length < 8) return E_PARAM;
            matchingDimensions = metadata.height == uint32_t(jpeg[position + 5]) * 256 + jpeg[position + 6] &&
                    metadata.width == uint32_t(jpeg[position + 7]) * 256 + jpeg[position + 8];
        }
        const bool exif = marker == 0xe1 && length >= 8 &&
                memcmp(jpeg.data() + position + 4, "Exif\0\0", 6) == 0;
        if (!exif) output.insert(output.end(), jpeg.begin() + position, jpeg.begin() + position + length + 2);
        position += length + 2;
    }
    if (position == jpeg.size() || !matchingDimensions) return E_PARAM;
    output.insert(output.end(), jpeg.begin() + position, jpeg.end());
    if (output.size() > metadata.maxJpegBytes) return E_ENCODE;
    jpeg.swap(output);
    return OK;
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

    // Reserve APP1 headroom so encoding cannot grow beyond the BLOB capacity.
    constexpr size_t kExifBytes = 160;
    std::vector<uint8_t> encoded(config.maxJpegBytes - kExifBytes);
    std::vector<JSAMPLE> row(size_t(config.width) * 3);
    jpeg_compress_struct compressor = {};
    ErrorManager error;
    jpeg_destination_mgr destination = {};
    destination.init_destination = initDestination;
    destination.empty_output_buffer = outputFull;
    destination.term_destination = finishDestination;
    destination.next_output_byte = encoded.data();
    destination.free_in_buffer = encoded.size();
    compressor.err = jpeg_std_error(&error.base);
    error.base.error_exit = onError;
    if (setjmp(error.jump) != 0) {
        jpeg_destroy_compress(&compressor);
        return E_ENCODE;
    }
    jpeg_create_compress(&compressor);
    compressor.dest = &destination;
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
    encoded.resize(encoded.size() - destination.free_in_buffer);
    if (!encoded.empty()) appendExifOrientation(encoded, exifOrientation(config.orientationDeg));
    jpeg.swap(encoded);
    return jpeg.empty() ? E_ENCODE : OK;
}

} // namespace qcamera
