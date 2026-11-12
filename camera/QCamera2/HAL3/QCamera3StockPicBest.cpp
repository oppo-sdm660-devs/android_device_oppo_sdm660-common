/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3StockPicBest.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <dlfcn.h>
#include <errno.h>
#include <limits>
#include <mutex>
#include <vector>

namespace qcamera {
namespace {
using PicBest = QCamera3StockPicBest;
// R11/R11s core 3ef68495: loadlib writes eighteen ARM32 handle/function slots.
struct Libraries { void *slots[18] = {}; };
struct FaceBlock {
    int32_t left[10], top[10], width[10], height[10];
    int32_t attribute[10];
    float roll[10];
    int32_t id[10], reserved[10];
    int32_t sourceWidth, sourceHeight, count;
};
static_assert(sizeof(FaceBlock) == 0x14c, "stock platform_face_data_t ABI");
static_assert(offsetof(FaceBlock, count) == 0x148, "stock face count ABI");
#if UINTPTR_MAX == UINT32_MAX
static_assert(sizeof(Libraries) == 0x48, "stock picbest_static_lib_t ABI");
#endif
std::mutex engineMutex;
struct Api {
    void *core = nullptr;
    Libraries libraries;
    int32_t (*load)(Libraries *) = nullptr;
    int32_t (*unload)(Libraries *) = nullptr;
    int32_t (*eyes)(Libraries *, FaceBlock *, int32_t, int32_t, int32_t, int32_t,
            uint8_t **, uint8_t **, int32_t (*)[10], int32_t (*)[4], int32_t) = nullptr;
    int32_t (*fuse)(Libraries *, int32_t, int32_t, int32_t, int32_t,
            uint8_t **, uint8_t **, int32_t, FaceBlock *, int32_t (*)[10]) = nullptr;
    bool open(const char *path) {
        if (!path || path[0] != '/') return false;
        core = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (!core) return false;
        load = reinterpret_cast<decltype(load)>(dlsym(core, "_Z15picbest_loadlibP20picbest_static_lib_t"));
        unload = reinterpret_cast<decltype(unload)>(dlsym(core, "_Z17picbest_unloadlibP20picbest_static_lib_t"));
        eyes = reinterpret_cast<decltype(eyes)>(dlsym(core,
                "_Z21executeEyesOpenDetectP20picbest_static_lib_tP20platform_face_data_tiiiiPPhS4_PA10_iPA4_ii"));
        fuse = reinterpret_cast<decltype(fuse)>(dlsym(core,
                "_Z14picBestProcessP20picbest_static_lib_tiiiiPPhS2_iP20platform_face_data_tPA10_i"));
        if (!load || !unload || !eyes || load(&libraries) != 0) return false;
        for (void *slot : libraries.slots) if (!slot) return false;
        return true;
    }
    ~Api() {
        if (unload) unload(&libraries);
        if (core) dlclose(core);
    }
};
bool cancelled(const std::atomic<bool> *flag) { return flag && flag->load(); }
bool validFace(const PicBest::Face &face, const PicBest::Image &image) {
    return face.id >= 0 && face.left >= 0 && face.top >= 0 && face.width > 0 && face.height > 0 &&
            int64_t(face.left) + face.width <= image.width &&
            int64_t(face.top) + face.height <= image.height && std::isfinite(face.rollDegrees) &&
            face.rollDegrees >= -180 && face.rollDegrees <= 180;
}
}

bool QCamera3StockPicBest::isAvailable(const char *corePath) {
    std::lock_guard<std::mutex> lock(engineMutex);
    Api api;
    return api.open(corePath);
}

bool QCamera3StockPicBest::isFusionAvailable(const char *corePath) {
    std::lock_guard<std::mutex> lock(engineMutex);
    Api api;
    return api.open(corePath) && api.fuse;
}

int32_t QCamera3StockPicBest::select(const Frame *frames, uint32_t count, Selection &result,
        const std::atomic<bool> *cancelFlag, const char *corePath) {
    return process(frames, count, result, nullptr, cancelFlag, corePath);
}

int32_t QCamera3StockPicBest::fuse(const Frame *frames, uint32_t count, const Image &output,
        Selection &result, const std::atomic<bool> *cancelFlag, const char *corePath) {
    return process(frames, count, result, &output, cancelFlag, corePath);
}

int32_t QCamera3StockPicBest::fuseNv21(const Frame *frames, uint32_t count, const Image &output,
        Selection &result, const std::atomic<bool> *cancelFlag, const char *corePath) {
    return process(frames, count, result, &output, cancelFlag, corePath, true);
}

int32_t QCamera3StockPicBest::process(const Frame *frames, uint32_t count, Selection &result,
        const Image *output, const std::atomic<bool> *cancelFlag, const char *corePath, bool nv21) {
    result = {};
    if (!frames || count < 2 || count > kMaxFrames) return -EINVAL;
    const uint32_t width = frames[0].image.width, height = frames[0].image.height;
    if (output && (!QCamera3HdrAlgo::validImage(*output) ||
            output->width != width || output->height != height)) return -EINVAL;
    for (uint32_t n = 0; n < count; ++n) {
        const auto &frame = frames[n];
        if (!QCamera3HdrAlgo::validImage(frame.image) || frame.image.width != width ||
                frame.image.height != height || frame.faceCount > kMaxFaces ||
                frame.timestampNs <= 0) return -EINVAL;
        for (uint32_t previous = 0; previous < n; ++previous)
            if (frame.timestampNs == frames[previous].timestampNs ||
                    frame.frameNumber == frames[previous].frameNumber) return -EINVAL;
        for (uint32_t face = 0; face < frame.faceCount; ++face) {
            if (!validFace(frame.faces[face], frame.image)) return -EINVAL;
            for (uint32_t previous = 0; previous < face; ++previous)
                if (frame.faces[face].id == frame.faces[previous].id) return -EINVAL;
        }
    }
    uint32_t common[kMaxFaces], commonCount = 0;
    for (uint32_t face = 0; face < frames[0].faceCount; ++face) {
        bool present = true;
        for (uint32_t n = 1; n < count; ++n) {
            bool found = false;
            for (uint32_t f = 0; f < frames[n].faceCount; ++f)
                found |= frames[n].faces[f].id == frames[0].faces[face].id;
            present &= found;
        }
        if (present) common[commonCount++] = face;
    }
    if (!commonCount) return -ENODATA;
    if (cancelled(cancelFlag)) return -ECANCELED;
    const uint64_t sourceBytes = uint64_t(width) * height * 3 / 2;
    if (sourceBytes > 256u * 1024u * 1024u / count) return -EOVERFLOW;
    const size_t frameBytes = sourceBytes;
    std::vector<uint8_t> packed(frameBytes * count);
    FaceBlock blocks[kMaxFrames] = {};
    uint8_t *y[kMaxFrames] = {}, *uv[kMaxFrames] = {};
    int32_t regions[kMaxFrames][4] = {};
    for (uint32_t n = 0; n < count; ++n) {
        const auto &frame = frames[n];
        y[n] = packed.data() + n * frameBytes;
        uv[n] = y[n] + size_t(width) * height;
        for (uint32_t row = 0; row < height; ++row)
            memcpy(y[n] + size_t(row) * width, frame.image.data + frame.image.yOffset +
                    size_t(row) * frame.image.yStride, width);
        // The core eye adapter constructs NV12 ASVLOFFSCREEN format 0x802.
        for (uint32_t row = 0; row < height / 2; ++row)
            memcpy(uv[n] + size_t(row) * width, frame.image.data + frame.image.uvOffset +
                    size_t(row) * frame.image.uvStride, width);
        if (nv21)
            for (size_t index = 0; index < size_t(width) * height / 2; index += 2)
                std::swap(uv[n][index], uv[n][index + 1]);
        auto &block = blocks[n];
        block.sourceWidth = width; block.sourceHeight = height; block.count = commonCount;
        for (uint32_t f = 0; f < commonCount; ++f) {
            const int32_t id = frames[0].faces[common[f]].id;
            const auto *face = std::find_if(frame.faces, frame.faces + frame.faceCount,
                    [id](const Face &candidate) { return candidate.id == id; });
            block.left[f] = face->left; block.top[f] = face->top;
            block.width[f] = face->width; block.height[f] = face->height;
            block.roll[f] = face->rollDegrees; block.id[f] = id;
        }
    }
    Selection selected;
    for (auto &row : selected.eyeScores) std::fill(std::begin(row), std::end(row), -1);
    std::lock_guard<std::mutex> lock(engineMutex);
    if (cancelled(cancelFlag)) return -ECANCELED;
    Api api;
    if (!api.open(corePath) || (output && !api.fuse)) return -ENOSYS;
    const int32_t rc = api.eyes(&api.libraries, blocks, width, height, width, height,
            y, uv, selected.eyeScores, regions, count);
    if (cancelled(cancelFlag)) return -ECANCELED;
    if (rc != 1) return -EIO;
    int32_t bestMinimum = -1;
    int64_t bestSum = -1;
    bool found = false;
    for (uint32_t n = 0; n < count; ++n) {
        int32_t minimum = std::numeric_limits<int32_t>::max();
        int64_t sum = 0;
        bool complete = true;
        for (uint32_t f = 0; f < commonCount; ++f) {
            const int32_t score = selected.eyeScores[n][f];
            if (score < 0) complete = false;
            minimum = std::min(minimum, score);
            sum += score;
        }
        // Prefer all subjects' eyes, then aggregate score, then the newer source.
        if (complete && (!found || minimum > bestMinimum ||
                (minimum == bestMinimum && sum >= bestSum))) {
            found = true; bestMinimum = minimum; bestSum = sum; selected.index = n;
        }
    }
    if (!found) return -ENODATA;
    selected.frameNumber = frames[selected.index].frameNumber;
    selected.timestampNs = frames[selected.index].timestampNs;
    selected.matchedFaces = commonCount;
    for (uint32_t f = 0; f < commonCount; ++f)
        selected.faceIds[f] = frames[0].faces[common[f]].id;
    if (output) {
        const int32_t status = api.fuse(&api.libraries, width, height, width, height,
                y, uv, count, blocks, selected.eyeScores);
        if (status != 0) return status < 0 ? status : -EIO;
        if (cancelled(cancelFlag)) return -ECANCELED;
        bool changed = false;
        for (uint32_t row = 0; row < height; ++row) {
            changed |= memcmp(frames[0].image.data + frames[0].image.yOffset +
                    size_t(row) * frames[0].image.yStride, y[0] + size_t(row) * width, width) != 0;
            memcpy(output->data + output->yOffset + size_t(row) * output->yStride,
                    y[0] + size_t(row) * width, width);
        }
        if (nv21)
            for (size_t index = 0; index < size_t(width) * height / 2; index += 2)
                std::swap(uv[0][index], uv[0][index + 1]);
        for (uint32_t row = 0; row < height / 2; ++row) {
            changed |= memcmp(frames[0].image.data + frames[0].image.uvOffset +
                    size_t(row) * frames[0].image.uvStride, uv[0] + size_t(row) * width, width) != 0;
            memcpy(output->data + output->uvOffset + size_t(row) * output->uvStride,
                    uv[0] + size_t(row) * width, width);
        }
        selected.fusionApplied = changed;
        selected.index = 0;
        selected.frameNumber = frames[0].frameNumber;
        selected.timestampNs = frames[0].timestampNs;
    }
    result = selected;
    return 0;
}

} // namespace qcamera
