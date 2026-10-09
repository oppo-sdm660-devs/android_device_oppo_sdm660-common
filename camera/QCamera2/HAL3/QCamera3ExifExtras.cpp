/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3ExifExtras.h"

#include <cmath>
#include <errno.h>
#include <new>
#include <stdlib.h>
#include <string.h>

namespace qcamera {
namespace {

using Options = QCamera3ExifExtras::Options;
using Result = QCamera3ExifExtras::Result;

// EXIF/TIFF vocabulary used by this writer.
constexpr uint16_t kTagImageDescription = 0x010E;
constexpr uint16_t kTagExifIfdPointer = 0x8769;
constexpr uint16_t kTagGpsIfdPointer = 0x8825;
constexpr uint16_t kTagInteropIfdPointer = 0xA005;
constexpr uint16_t kTagUserComment = 0x9286;
constexpr uint8_t kTypeAscii = 2;
constexpr uint8_t kTypeLong = 4;
constexpr uint8_t kTypeUndefined = 7;
constexpr size_t kMaxIfds = QCamera3ExifExtras::MAX_IFDS;
constexpr size_t kMaxIfdEntries = QCamera3ExifExtras::MAX_IFD_ENTRIES;
constexpr size_t kMaxSegments = QCamera3ExifExtras::MAX_SEGMENTS;
constexpr size_t kMaxJpegBytes = QCamera3ExifExtras::MAX_JPEG_BYTES;
constexpr size_t kMaxGrowth = QCamera3ExifExtras::MAX_GROWTH_BYTES;
constexpr uint32_t kMaxApp1Payload = QCamera3ExifExtras::MAX_APP1_PAYLOAD;
constexpr size_t kMaxCommentText = QCamera3ExifExtras::MAX_COMMENT_TEXT;
constexpr size_t kMaxDescription = QCamera3ExifExtras::MAX_DESCRIPTION;

bool stopped(const std::atomic<bool> *flag) { return flag && flag->load(); }

uint32_t typeSize(uint16_t type) {
    switch (type) {
    case 1: case 2: case 6: case 7: return 1;      // BYTE ASCII SBYTE UNDEFINED
    case 3: case 8: return 2;                      // SHORT SSHORT
    case 4: case 9: case 11: return 4;             // LONG SLONG FLOAT
    case 5: case 10: case 12: return 8;            // RATIONAL SRATIONAL DOUBLE
    default: return 0;
    }
}

uint16_t read16(const uint8_t *data, bool little) {
    return little ? uint16_t(data[0] | uint16_t(data[1]) << 8)
                  : uint16_t(uint16_t(data[0]) << 8 | data[1]);
}

uint32_t read32(const uint8_t *data, bool little) {
    if (little) {
        return uint32_t(data[0]) | uint32_t(data[1]) << 8 | uint32_t(data[2]) << 16 |
                uint32_t(data[3]) << 24;
    }
    return uint32_t(data[0]) << 24 | uint32_t(data[1]) << 16 | uint32_t(data[2]) << 8 |
            uint32_t(data[3]);
}

void write16(uint8_t *data, uint16_t value, bool little) {
    if (little) {
        data[0] = uint8_t(value & 0xFF);
        data[1] = uint8_t(value >> 8);
    } else {
        data[0] = uint8_t(value >> 8);
        data[1] = uint8_t(value & 0xFF);
    }
}

void write32(uint8_t *data, uint32_t value, bool little) {
    if (little) {
        data[0] = uint8_t(value & 0xFF);
        data[1] = uint8_t((value >> 8) & 0xFF);
        data[2] = uint8_t((value >> 16) & 0xFF);
        data[3] = uint8_t((value >> 24) & 0xFF);
    } else {
        data[0] = uint8_t((value >> 24) & 0xFF);
        data[1] = uint8_t((value >> 16) & 0xFF);
        data[2] = uint8_t((value >> 8) & 0xFF);
        data[3] = uint8_t(value & 0xFF);
    }
}

uint32_t align2(uint32_t value) { return (value + 1U) & ~1U; }

struct Entry {
    uint16_t tag = 0;
    uint16_t type = 0;
    uint32_t count = 0;
    uint32_t valueSize = 0;
    uint8_t value[4] = {};
    int32_t subIfd = -1;   // index into Tiff::ifds when this is a pointer tag
};

struct Ifd {
    uint32_t offset = 0;
    uint16_t count = 0;
    uint32_t next = 0;
    int32_t nextIfd = -1;
    Entry entries[kMaxIfdEntries];
};

struct Tiff {
    bool little = true;
    const uint8_t *data = nullptr;
    uint32_t length = 0;
    uint32_t ifd0Offset = 0;
    uint32_t ifdCount = 0;
    uint32_t entryCount = 0;
    Ifd ifds[kMaxIfds];
};

// Parses one IFD and, recursively, the sub-IFDs it points at; returns the index
// of the parsed IFD (>= 0) or a negative errno. The chain of IFD0 -> IFD1 is
// walked by parseTiff.
int32_t parseIfd(const uint8_t *data, uint32_t length, bool little, uint32_t offset,
        int32_t parent, uint16_t parentEntry, uint32_t depth, Tiff &tiff,
        const std::atomic<bool> *cancelled) {
    if (depth > 2) return -ENOTSUP;
    if (tiff.ifdCount >= kMaxIfds) return -E2BIG;
    if (offset < 8 || uint64_t(offset) + 2U > length) return -EMSGSIZE;
    for (uint32_t i = 0; i < tiff.ifdCount; ++i) {
        if (tiff.ifds[i].offset == offset) return -ENOTSUP; // no cycles
    }
    const uint32_t index = tiff.ifdCount++;
    Ifd &ifd = tiff.ifds[index];
    ifd.offset = offset;
    ifd.count = read16(data + offset, little);
    if (ifd.count > kMaxIfdEntries) return -E2BIG;
    const uint64_t end = uint64_t(offset) + 2U + uint64_t(ifd.count) * 12U + 4U;
    if (end > length) return -EMSGSIZE;
    for (uint16_t i = 0; i < ifd.count; ++i) {
        if ((i % 16U) == 0 && stopped(cancelled)) return -ECANCELED;
        const uint8_t *raw = data + offset + 2U + size_t(i) * 12U;
        Entry &entry = ifd.entries[i];
        entry.tag = read16(raw, little);
        entry.type = read16(raw + 2, little);
        entry.count = read32(raw + 4, little);
        memcpy(entry.value, raw + 8, 4);
        const uint32_t unit = typeSize(entry.type);
        if (!unit) return -EPROTONOSUPPORT;
        const uint64_t size = uint64_t(unit) * entry.count;
        if (size > 0xFFFFFFFFULL) return -EMSGSIZE;
        entry.valueSize = uint32_t(size);
        if (entry.valueSize > 4) {
            const uint32_t valueOffset = read32(entry.value, little);
            if (uint64_t(valueOffset) + entry.valueSize > length) return -EMSGSIZE;
        }
        ++tiff.entryCount;
    }
    ifd.next = read32(data + offset + 2U + size_t(ifd.count) * 12U, little);
    if (parent >= 0) tiff.ifds[parent].entries[parentEntry].subIfd = int32_t(index);
    for (uint16_t i = 0; i < ifd.count; ++i) {
        const uint16_t tag = ifd.entries[i].tag;
        if (tag != kTagExifIfdPointer && tag != kTagGpsIfdPointer &&
                tag != kTagInteropIfdPointer) continue;
        if (ifd.entries[i].type != kTypeLong || ifd.entries[i].count != 1 ||
                ifd.entries[i].valueSize != 4) return -EPROTONOSUPPORT;
        const uint32_t target = read32(ifd.entries[i].value, little);
        if (!target) continue; // a null pointer is preserved unchanged
        const int32_t sub = parseIfd(data, length, little, target, int32_t(index), i,
                depth + 1, tiff, cancelled);
        if (sub < 0) return sub;
    }
    return int32_t(index);
}

int32_t parseTiff(const uint8_t *data, uint32_t length, Tiff &tiff,
        const std::atomic<bool> *cancelled) {
    if (!data || length < 8) return -EMSGSIZE;
    if (data[0] == 'I' && data[1] == 'I') tiff.little = true;
    else if (data[0] == 'M' && data[1] == 'M') tiff.little = false;
    else return -EPROTONOSUPPORT;
    if (read16(data + 2, tiff.little) != 42) return -EPROTONOSUPPORT;
    tiff.data = data;
    tiff.length = length;
    tiff.ifd0Offset = read32(data + 4, tiff.little);
    uint32_t offset = tiff.ifd0Offset;
    int32_t previous = -1;
    while (offset) {
        const int32_t index = parseIfd(data, length, tiff.little, offset, -1, 0, 0, tiff,
                cancelled);
        if (index < 0) return index;
        if (previous >= 0) tiff.ifds[previous].nextIfd = index;
        previous = index;
        offset = tiff.ifds[index].next;
    }
    return tiff.ifdCount ? 0 : -EMSGSIZE;
}

struct Segment {
    size_t markerOffset = 0;
    size_t totalLength = 0;
    uint16_t marker = 0;
};

struct Layout {
    Segment segments[kMaxSegments];
    size_t segmentCount = 0;
    size_t app1Count = 0;
    size_t scanOffset = 0;
    size_t exifIndex = SIZE_MAX;
    size_t insertOffset = 2;
};

int32_t parseJpeg(const uint8_t *jpeg, size_t length, Layout &layout,
        const std::atomic<bool> *cancelled) {
    if (!jpeg || length < 4 || jpeg[0] != 0xFF || jpeg[1] != 0xD8) return -EINVAL;
    size_t offset = 2;
    bool foundScan = false;
    while (offset + 1 < length) {
        if ((layout.segmentCount % 32U) == 0 && stopped(cancelled)) return -ECANCELED;
        if (jpeg[offset] != 0xFF) return -EINVAL;
        const size_t markerOffset = offset;
        while (offset < length && jpeg[offset] == 0xFF) ++offset;
        if (offset >= length) return -EINVAL;
        const uint8_t marker = jpeg[offset++];
        if (marker == 0xD9) return -EINVAL; // EOI before any SOS: not a JPEG scan
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue;
        if (offset + 2 > length) return -EINVAL;
        const uint16_t segmentLength = read16(jpeg + offset, false);
        if (segmentLength < 2 || offset + segmentLength > length) return -EINVAL;
        if (marker == 0xDA) {
            layout.scanOffset = markerOffset;
            foundScan = true;
            return 0; // entropy-coded data follows and is copied verbatim
        }
        if (layout.segmentCount >= kMaxSegments) return -E2BIG;
        Segment &segment = layout.segments[layout.segmentCount];
        segment.markerOffset = markerOffset;
        segment.totalLength = (offset - markerOffset) + segmentLength;
        segment.marker = marker;
        const uint8_t *payload = jpeg + offset + 2;
        if (marker == 0xE1) {
            ++layout.app1Count;
            if (layout.exifIndex == SIZE_MAX && segmentLength >= 8 &&
                    !memcmp(payload, "Exif\0\0", 6)) layout.exifIndex = layout.segmentCount;
        }
        // A new APP1 goes after a leading JFIF APP0 when one exists.
        if (!layout.segmentCount && marker == 0xE0)
            layout.insertOffset = segment.markerOffset + segment.totalLength;
        ++layout.segmentCount;
        offset += segmentLength;
    }
    return foundScan ? 0 : -EINVAL;
}

// --- TIFF re-emission plan -------------------------------------------------

struct PlannedEntry {
    uint16_t tag = 0;
    uint16_t type = 0;
    uint32_t count = 0;
    uint32_t valueSize = 0;
    uint8_t value[4] = {};
    const uint8_t *newValue = nullptr;
    uint32_t newValueOffset = 0;
    uint32_t newValueSize = 0;
    int32_t subIfd = -1;
};

struct PlannedIfd {
    uint32_t offset = 0;
    uint16_t count = 0;
    int32_t nextIfd = -1;
    PlannedEntry entries[kMaxIfdEntries + 2];
};

struct Plan {
    bool little = true;
    bool created = false;
    uint32_t originalLength = 0;
    uint32_t ifd0Offset = 0;
    uint32_t newLength = 0;
    uint32_t growth = 0;
    uint32_t ifdCount = 0;
    PlannedIfd ifds[kMaxIfds];
    uint8_t comment[8 + kMaxCommentText] = {};
    uint32_t commentLength = 0;
    uint8_t description[kMaxDescription + 1] = {};
    uint32_t descriptionLength = 0;
    bool hasComment = false;
    bool hasDescription = false;
};

struct Workspace {
    Layout layout;
    Tiff tiff;
    Plan plan;
    bool hasTiff = false;
};

// Inserts `entry` keeping ascending tag order (stable for equal tags).
void insertEntry(PlannedIfd &ifd, const PlannedEntry &entry) {
    size_t at = ifd.count;
    while (at > 0 && ifd.entries[at - 1].tag > entry.tag) --at;
    for (size_t i = ifd.count; i > at; --i) ifd.entries[i] = ifd.entries[i - 1];
    ifd.entries[at] = entry;
    ++ifd.count;
}

int32_t buildPlan(const Tiff *tiff, const Options &options, bool wantDescription,
        bool wantComment, Plan &plan) {
    plan.created = tiff == nullptr;
    plan.originalLength = tiff ? tiff->length : 0;
    plan.little = tiff ? tiff->little : true;
    plan.ifdCount = tiff ? tiff->ifdCount : 1;
    if (plan.ifdCount > kMaxIfds) return -E2BIG;
    for (uint32_t i = 0; i < plan.ifdCount; ++i) {
        PlannedIfd &target = plan.ifds[i];
        target.nextIfd = tiff ? tiff->ifds[i].nextIfd : -1;
        if (!tiff) continue;
        if (tiff->ifds[i].count > kMaxIfdEntries) return -E2BIG;
        for (uint16_t e = 0; e < tiff->ifds[i].count; ++e) {
            const Entry &source = tiff->ifds[i].entries[e];
            PlannedEntry entry;
            entry.tag = source.tag;
            entry.type = source.type;
            entry.count = source.count;
            entry.valueSize = source.valueSize;
            memcpy(entry.value, source.value, 4);
            entry.subIfd = source.subIfd;
            insertEntry(target, entry);
        }
    }

    // Locate the Exif sub-IFD of IFD0 (or create one when a comment is wanted).
    int32_t exifIfd = -1;
    for (uint16_t e = 0; e < plan.ifds[0].count; ++e) {
        if (plan.ifds[0].entries[e].tag == kTagExifIfdPointer)
            exifIfd = plan.ifds[0].entries[e].subIfd;
    }
    if (wantComment && exifIfd < 0) {
        if (plan.ifdCount >= kMaxIfds) return -E2BIG;
        exifIfd = int32_t(plan.ifdCount++);
        plan.ifds[exifIfd].nextIfd = -1;
        PlannedEntry pointer;
        pointer.tag = kTagExifIfdPointer;
        pointer.type = kTypeLong;
        pointer.count = 1;
        pointer.valueSize = 4;
        pointer.subIfd = exifIfd;
        insertEntry(plan.ifds[0], pointer);
    }

    if (wantDescription) {
        const size_t length = strlen(options.imageDescription);
        if (length > kMaxDescription) return -E2BIG;
        memcpy(plan.description, options.imageDescription, length + 1);
        plan.descriptionLength = uint32_t(length + 1);
        plan.hasDescription = true;
        PlannedEntry entry;
        entry.tag = kTagImageDescription;
        entry.type = kTypeAscii;
        entry.count = uint32_t(length + 1);
        entry.valueSize = entry.count;
        entry.newValue = plan.description;
        entry.newValueSize = entry.count;
        bool replaced = false;
        for (uint16_t e = 0; e < plan.ifds[0].count; ++e) {
            if (plan.ifds[0].entries[e].tag != kTagImageDescription) continue;
            plan.ifds[0].entries[e].type = entry.type;
            plan.ifds[0].entries[e].count = entry.count;
            plan.ifds[0].entries[e].valueSize = entry.valueSize;
            plan.ifds[0].entries[e].newValue = entry.newValue;
            plan.ifds[0].entries[e].newValueSize = entry.newValueSize;
            replaced = true;
            break;
        }
        if (!replaced) insertEntry(plan.ifds[0], entry);
    }

    if (wantComment) {
        const size_t length = strlen(options.userComment);
        if (length > kMaxCommentText) return -E2BIG;
        static const char prefix[8] = {'A', 'S', 'C', 'I', 'I', 0, 0, 0};
        memcpy(plan.comment, prefix, sizeof(prefix));
        memcpy(plan.comment + 8, options.userComment, length);
        plan.commentLength = uint32_t(8 + length);
        plan.hasComment = true;
        PlannedEntry entry;
        entry.tag = kTagUserComment;
        entry.type = kTypeUndefined;
        entry.count = plan.commentLength;
        entry.valueSize = plan.commentLength;
        entry.newValue = plan.comment;
        entry.newValueSize = plan.commentLength;
        bool replaced = false;
        for (uint16_t e = 0; e < plan.ifds[exifIfd].count; ++e) {
            if (plan.ifds[exifIfd].entries[e].tag != kTagUserComment) continue;
            plan.ifds[exifIfd].entries[e].type = entry.type;
            plan.ifds[exifIfd].entries[e].count = entry.count;
            plan.ifds[exifIfd].entries[e].valueSize = entry.valueSize;
            plan.ifds[exifIfd].entries[e].newValue = entry.newValue;
            plan.ifds[exifIfd].entries[e].newValueSize = entry.newValueSize;
            replaced = true;
            break;
        }
        if (!replaced) insertEntry(plan.ifds[exifIfd], entry);
    }

    // Lay out the re-emitted structures, then the new values. The original
    // payload keeps its offsets, so every original value block stays valid.
    uint32_t offset = plan.originalLength ? align2(plan.originalLength) : 8U;
    for (uint32_t i = 0; i < plan.ifdCount; ++i) {
        PlannedIfd &ifd = plan.ifds[i];
        if (ifd.count > kMaxIfdEntries + 2) return -E2BIG;
        ifd.offset = offset;
        offset += 2U + 12U * ifd.count + 4U;
    }
    uint32_t values = align2(offset);
    if (plan.hasDescription) {
        for (uint32_t i = 0; i < plan.ifdCount; ++i) {
            for (uint16_t e = 0; e < plan.ifds[i].count; ++e) {
                PlannedEntry &entry = plan.ifds[i].entries[e];
                if (entry.tag == kTagImageDescription && entry.newValue) {
                    entry.newValueOffset = values;
                }
            }
        }
        values += align2(plan.descriptionLength);
    }
    if (plan.hasComment) {
        for (uint32_t i = 0; i < plan.ifdCount; ++i) {
            for (uint16_t e = 0; e < plan.ifds[i].count; ++e) {
                PlannedEntry &entry = plan.ifds[i].entries[e];
                if (entry.tag == kTagUserComment && entry.newValue) {
                    entry.newValueOffset = values;
                }
            }
        }
        values += align2(plan.commentLength);
    }
    plan.ifd0Offset = plan.ifds[0].offset;
    plan.newLength = values;
    plan.growth = plan.newLength - plan.originalLength;
    if (plan.growth > kMaxGrowth) return -E2BIG;
    if (6U + plan.newLength > kMaxApp1Payload) return -E2BIG;
    return 0;
}

void writeTiff(uint8_t *out, const Plan &plan, const Tiff *tiff) {
    out[0] = plan.little ? 'I' : 'M';
    out[1] = out[0];
    write16(out + 2, 42, plan.little);
    write32(out + 4, plan.ifd0Offset, plan.little);
    if (plan.originalLength) {
        memcpy(out + 8, tiff->data + 8, plan.originalLength - 8);
        const uint32_t base = align2(plan.originalLength);
        if (base > plan.originalLength)
            memset(out + plan.originalLength, 0, base - plan.originalLength);
    }
    for (uint32_t i = 0; i < plan.ifdCount; ++i) {
        const PlannedIfd &ifd = plan.ifds[i];
        uint8_t *raw = out + ifd.offset;
        write16(raw, ifd.count, plan.little);
        for (uint16_t e = 0; e < ifd.count; ++e) {
            const PlannedEntry &entry = ifd.entries[e];
            uint8_t *slot = raw + 2U + size_t(e) * 12U;
            write16(slot, entry.tag, plan.little);
            write16(slot + 2, entry.type, plan.little);
            write32(slot + 4, entry.count, plan.little);
            if (entry.newValue) {
                if (entry.valueSize > 4) {
                    write32(slot + 8, entry.newValueOffset, plan.little);
                } else {
                    // Short ASCII values stay inline in the entry.
                    uint8_t inlineBytes[4] = {};
                    memcpy(inlineBytes, entry.newValue, entry.valueSize);
                    memcpy(slot + 8, inlineBytes, 4);
                }
            } else if (entry.subIfd >= 0) {
                write32(slot + 8, plan.ifds[entry.subIfd].offset, plan.little);
            } else {
                memcpy(slot + 8, entry.value, 4);
            }
        }
        write32(raw + 2U + size_t(ifd.count) * 12U,
                ifd.nextIfd >= 0 ? plan.ifds[ifd.nextIfd].offset : 0U, plan.little);
    }
    if (plan.hasDescription) {
        for (uint32_t i = 0; i < plan.ifdCount; ++i) {
            for (uint16_t e = 0; e < plan.ifds[i].count; ++e) {
                const PlannedEntry &entry = plan.ifds[i].entries[e];
                if (entry.tag == kTagImageDescription && entry.newValue)
                    memcpy(out + entry.newValueOffset, plan.description, plan.descriptionLength);
            }
        }
    }
    if (plan.hasComment) {
        for (uint32_t i = 0; i < plan.ifdCount; ++i) {
            for (uint16_t e = 0; e < plan.ifds[i].count; ++e) {
                const PlannedEntry &entry = plan.ifds[i].entries[e];
                if (entry.tag == kTagUserComment && entry.newValue)
                    memcpy(out + entry.newValueOffset, plan.comment, plan.commentLength);
            }
        }
    }
}

void writeApp1(uint8_t *out, const Plan &plan, const Tiff *tiff) {
    out[0] = 0xFF;
    out[1] = 0xE1;
    write16(out + 2, uint16_t(2U + 6U + plan.newLength), false);
    memcpy(out + 4, "Exif\0\0", 6);
    writeTiff(out + 10, plan, tiff);
}

} // namespace

bool QCamera3ExifExtras::Options::valid(const Options &o) {
    if (o.imageDescription) {
        const size_t length = strnlen(o.imageDescription, kMaxDescription + 1);
        if (length > kMaxDescription) return false;
        for (size_t i = 0; i < length; ++i) {
            const unsigned char c = static_cast<unsigned char>(o.imageDescription[i]);
            if (c < 0x20 || c > 0x7E) return false;
        }
    }
    if (o.userComment) {
        const size_t length = strnlen(o.userComment, kMaxCommentText + 1);
        if (length > kMaxCommentText) return false;
        if (!validUtf8(reinterpret_cast<const uint8_t *>(o.userComment), length)) return false;
    }
    return true;
}

bool QCamera3ExifExtras::validUtf8(const uint8_t *text, size_t length) {
    if (!text && length) return false;
    size_t i = 0;
    while (i < length) {
        const uint8_t first = text[i];
        if (first == 0x00) return false;
        if (first < 0x80) {
            // Reject C0 controls except tab, line feed and carriage return.
            if (first < 0x20 && first != 0x09 && first != 0x0A && first != 0x0D) return false;
            ++i;
            continue;
        }
        size_t extra = 0;
        uint8_t secondMin = 0x80, secondMax = 0xBF;
        if (first >= 0xC2 && first <= 0xDF) extra = 1;
        else if (first == 0xE0) { extra = 2; secondMin = 0xA0; }
        else if (first >= 0xE1 && first <= 0xEC) extra = 2;
        else if (first == 0xED) { extra = 2; secondMax = 0x9F; } // no surrogates
        else if (first >= 0xEE && first <= 0xEF) extra = 2;
        else if (first == 0xF0) { extra = 3; secondMin = 0x90; }
        else if (first >= 0xF1 && first <= 0xF3) extra = 3;
        else if (first == 0xF4) { extra = 3; secondMax = 0x8F; }
        else return false; // continuation byte, overlong C0/C1, 0xF5..0xFF
        if (i + extra >= length) return false;
        if (text[i + 1] < secondMin || text[i + 1] > secondMax) return false;
        for (size_t k = 2; k <= extra; ++k) {
            if (text[i + k] < 0x80 || text[i + k] > 0xBF) return false;
        }
        i += extra + 1;
    }
    return true;
}

const char *QCamera3ExifExtras::name() { return "ExifExtras"; }

bool QCamera3ExifExtras::isAvailable() { return true; }

bool QCamera3ExifExtras::validOptions(const Options &options) { return Options::valid(options); }

size_t QCamera3ExifExtras::maxOutputLength(size_t sourceLength) {
    if (sourceLength > kMaxJpegBytes) return 0;
    return sourceLength + kMaxGrowth + 8U;
}

int32_t QCamera3ExifExtras::describe(const uint8_t *jpeg, size_t length, JpegInfo &info) {
    info = {};
    if (!jpeg || !length || length > kMaxJpegBytes) return -EINVAL;
    Workspace *workspace = new (std::nothrow) Workspace;
    if (!workspace) return -ENOMEM;
    int32_t status = parseJpeg(jpeg, length, workspace->layout, nullptr);
    if (!status) {
        info.segmentCount = workspace->layout.segmentCount;
        info.app1Count = workspace->layout.app1Count;
        info.scanOffset = workspace->layout.scanOffset;
        if (workspace->layout.exifIndex != SIZE_MAX) {
            const Segment &segment = workspace->layout.segments[workspace->layout.exifIndex];
            info.hasExif = 1;
            info.exifMarkerOffset = segment.markerOffset;
            info.exifSegmentLength = segment.totalLength;
            const uint8_t *payload = jpeg + segment.markerOffset + 4;
            const uint32_t payloadLength = uint32_t(segment.totalLength - 4);
            if (payloadLength >= 6) {
                status = parseTiff(payload + 6, payloadLength - 6, workspace->tiff, nullptr);
                if (!status) {
                    info.ifdCount = workspace->tiff.ifdCount;
                    info.entryCount = workspace->tiff.entryCount;
                }
            }
        }
    }
    delete workspace;
    return status;
}

int32_t QCamera3ExifExtras::process(const uint8_t *source, size_t sourceLength,
        uint8_t *destination, size_t capacity, size_t &destinationLength,
        const Options &options, Result &result, const std::atomic<bool> *cancelled) {
    result = {};
    destinationLength = 0;
    result.frameNumber = options.frameNumber;
    result.sourceLength = sourceLength;
    if (!source || !destination || !sourceLength) {
        result.status = -EINVAL;
        return -EINVAL;
    }
    if (sourceLength > kMaxJpegBytes) {
        result.status = -E2BIG;
        return -E2BIG;
    }
    if ((options.userComment &&
                strnlen(options.userComment, kMaxCommentText + 1) > kMaxCommentText) ||
            (options.imageDescription &&
                strnlen(options.imageDescription, kMaxDescription + 1) > kMaxDescription)) {
        result.status = -E2BIG;
        return -E2BIG;
    }
    if (!validOptions(options)) {
        result.status = -EINVAL;
        return -EINVAL;
    }
    const bool wantDescription = options.imageDescription && options.imageDescription[0];
    const bool wantComment = options.userComment && options.userComment[0];
    if (!wantDescription && !wantComment) {
        // Byte-exact preservation: nothing is written, the caller keeps source.
        result.bypassed = 1;
        destinationLength = sourceLength;
        return 0;
    }

    Workspace *workspace = new (std::nothrow) Workspace;
    if (!workspace) {
        result.status = -ENOMEM;
        return -ENOMEM;
    }
    int32_t status = parseJpeg(source, sourceLength, workspace->layout, cancelled);
    if (!status && workspace->layout.exifIndex != SIZE_MAX) {
        const Segment &segment = workspace->layout.segments[workspace->layout.exifIndex];
        const uint8_t *payload = source + segment.markerOffset + 4;
        const uint32_t payloadLength = uint32_t(segment.totalLength - 4);
        status = parseTiff(payload + 6, payloadLength - 6, workspace->tiff, cancelled);
        workspace->hasTiff = status == 0;
    }
    if (!status)
        status = buildPlan(workspace->hasTiff ? &workspace->tiff : nullptr, options,
                wantDescription, wantComment, workspace->plan);
    if (!status) {
        const size_t newSegmentLength = 4U + 6U + workspace->plan.newLength;
        const size_t removed = workspace->layout.exifIndex != SIZE_MAX
                ? workspace->layout.segments[workspace->layout.exifIndex].totalLength : 0U;
        const size_t outputLength = sourceLength - removed + newSegmentLength;
        if (outputLength > capacity) status = -E2BIG;
        else if (stopped(cancelled)) status = -ECANCELED;
        else {
            if (workspace->layout.exifIndex != SIZE_MAX) {
                const Segment &segment = workspace->layout.segments[workspace->layout.exifIndex];
                memcpy(destination, source, segment.markerOffset);
                writeApp1(destination + segment.markerOffset, workspace->plan,
                        workspace->hasTiff ? &workspace->tiff : nullptr);
                memcpy(destination + segment.markerOffset + newSegmentLength,
                        source + segment.markerOffset + segment.totalLength,
                        sourceLength - segment.markerOffset - segment.totalLength);
            } else {
                const size_t insert = workspace->layout.insertOffset;
                memcpy(destination, source, insert);
                writeApp1(destination + insert, workspace->plan, nullptr);
                memcpy(destination + insert + newSegmentLength, source + insert,
                        sourceLength - insert);
            }
            destinationLength = outputLength;
            result.status = 0;
            result.applied = 1;
            result.outputLength = outputLength;
            result.segments = uint32_t(workspace->layout.segmentCount);
            result.ifds = workspace->plan.ifdCount;
            result.app1Length = uint32_t(6U + workspace->plan.newLength);
            result.createdApp1 = workspace->hasTiff ? 0 : 1;
            result.structureChanged = 1;
            result.detail[0] = int32_t(workspace->layout.segmentCount);
            result.detail[1] = int32_t(result.app1Length);
            result.detail[2] = int32_t(workspace->plan.ifdCount);
            result.detail[3] = int32_t(workspace->plan.growth);
        }
    }
    if (status) result.status = status;
    delete workspace;
    return status;
}

int32_t QCamera3ExifExtras::professionalVer() { return 1; }

int32_t QCamera3ExifExtras::estimateSnapshotExpTimeNs(const SnapshotExposureInput &input,
        int64_t &outNs) {
    if (input.sensorExposureTimeNs <= 0 || input.frameDurationNs <= 0 ||
            input.maxExposureTimeNs <= 0) return -ENODATA; // no real measured value
    if (input.aeState != AE_STATE_CONVERGED && input.aeState != AE_STATE_LOCKED) {
        // Searching/precapture: refuse to guess a future exposure decision.
        return -EAGAIN;
    }
    int64_t estimate = input.sensorExposureTimeNs;
    if (estimate > input.frameDurationNs) estimate = input.frameDurationNs;
    if (estimate > input.maxExposureTimeNs) estimate = input.maxExposureTimeNs;
    if (estimate <= 0) return -ENODATA;
    outNs = estimate;
    return 0;
}

int32_t QCamera3ExifExtras::lensFocusDistance(float diopters, float minDiopters,
        float maxDiopters, float &out) {
    if (!std::isfinite(diopters) || !std::isfinite(minDiopters) ||
            !std::isfinite(maxDiopters)) return -EINVAL;
    if (diopters < 0.0f || minDiopters < 0.0f || maxDiopters <= minDiopters) return -EINVAL;
    if (diopters < minDiopters) {
        out = minDiopters;
        return -ERANGE;
    }
    if (diopters > maxDiopters) {
        out = maxDiopters;
        return -ERANGE;
    }
    out = diopters;
    return 0;
}

} // namespace qcamera
