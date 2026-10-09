/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3HdrDenoise.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

namespace qcamera {
namespace {
constexpr size_t kHeader = 256, kRecord = 4108, kMaxRecords = 256;
constexpr uint32_t kWorkspace = 32U * 1024U * 1024U;
struct ArcImage {
    uint32_t format;
    int32_t width, height;
    uint8_t *planes[4];
    int32_t pitches[4];
};
#if UINTPTR_MAX == UINT32_MAX
static_assert(sizeof(ArcImage) == 44, "ASD ARM32 image ABI");
#endif
pthread_mutex_t gAsdLock = PTHREAD_MUTEX_INITIALIZER;
struct Lock {
    Lock() { pthread_mutex_lock(&gAsdLock); }
    ~Lock() { pthread_mutex_unlock(&gAsdLock); }
};
struct Api {
    void *asd = dlopen("libarcsoft_smart_denoise.so", RTLD_NOW | RTLD_LOCAL);
    void *mp = dlopen("libmpbase.so", RTLD_NOW | RTLD_LOCAL);
    void *(*managerCreate)(void *, uint32_t) = nullptr;
    void (*managerDestroy)(void *) = nullptr;
    int32_t (*create)(void *, void **) = nullptr;
    int32_t (*release)(void *) = nullptr;
    int32_t (*type)(void *, int32_t) = nullptr;
    int32_t (*config)(void *, void *) = nullptr;
    int32_t (*luminance)(void *, int32_t) = nullptr;
    int32_t (*color)(void *, int32_t) = nullptr;
    int32_t (*sharp)(void *, int32_t) = nullptr;
    int32_t (*ring)(void *, int32_t) = nullptr;
    int32_t (*denoise)(void *, ArcImage *, ArcImage *, void *, void *) = nullptr;
    Api() {
#define LOAD(lib, member, symbol) member = reinterpret_cast<decltype(member)>(lib ? dlsym(lib, symbol) : nullptr)
        LOAD(mp, managerCreate, "MMemMgrCreate");
        LOAD(mp, managerDestroy, "MMemMgrDestroy");
        LOAD(asd, create, "ASD_CreateHandle");
        LOAD(asd, release, "ASD_ReleaseHandle");
        LOAD(asd, type, "ASD_SetDenoiserType");
        LOAD(asd, config, "ASD_NoiseConfigFromMemory");
        LOAD(asd, luminance, "ASD_SetLuminDenoiseLevel");
        LOAD(asd, color, "ASD_SetColorDenoiseLevel");
        LOAD(asd, sharp, "ASD_SetSharpLevel");
        LOAD(asd, ring, "ASD_SetRingNoiseLevel");
        LOAD(asd, denoise, "ASD_Denoise");
#undef LOAD
    }
    ~Api() { if (asd) dlclose(asd); if (mp) dlclose(mp); }
    bool valid() const {
        return managerCreate && managerDestroy && create && release && type &&
                config && luminance && color && sharp && ring && denoise;
    }
};
struct Buffer {
    uint8_t *data;
    explicit Buffer(size_t n) : data(static_cast<uint8_t *>(calloc(1, n))) {}
    ~Buffer() { free(data); }
};
uint32_t read32(const uint8_t *p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
            (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
bool canceled(const std::atomic<bool> *flag) { return flag && flag->load(); }
void copyRows(uint8_t *dst, uint32_t dp, const uint8_t *src, uint32_t sp,
        uint32_t width, uint32_t rows) {
    for (uint32_t y = 0; y < rows; ++y)
        memcpy(dst + size_t(y) * dp, src + size_t(y) * sp, width);
}
int32_t loadProfile(const char *path, const QCamera3HdrAlgo::Image &image,
        int32_t iso, QCamera3HdrDenoise::Profile &profile) {
    if (!path) return -EINVAL;
    const int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return -errno;
    struct stat info = {};
    if (fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_size < off_t(kHeader + kRecord) ||
            info.st_size > off_t(kHeader + kRecord * kMaxRecords)) {
        close(fd);
        return -EINVAL;
    }
    const size_t size = size_t(info.st_size);
    Buffer bytes(size);
    if (!bytes.data) { close(fd); return -ENOMEM; }
    size_t done = 0;
    while (done < size) {
        const ssize_t n = read(fd, bytes.data + done, size - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(fd); return -EIO; }
        done += size_t(n);
    }
    close(fd);
    return QCamera3HdrDenoise::selectProfile(bytes.data, size,
            image.width, image.height, iso, profile);
}
}

int32_t QCamera3HdrDenoise::selectProfile(const void *pack, size_t length,
        uint32_t width, uint32_t height, int32_t iso, Profile &profile) {
    memset(&profile, 0, sizeof(profile));
    if (!pack || !width || !height || iso <= 0 || length < kHeader + kRecord ||
            length > kHeader + kRecord * kMaxRecords ||
            (length - kHeader) % kRecord) return -EINVAL;
    const auto *bytes = static_cast<const uint8_t *>(pack);
    const size_t count = (length - kHeader) / kRecord;
    // HAL1 selects the matching resolution group or the first one, then floors ISO.
    size_t first = 0;
    for (size_t i = 0; i < count; ++i) {
        const uint8_t *r = bytes + kHeader + i * kRecord;
        if (read32(r + 4) == width && read32(r + 8) == height) { first = i; break; }
    }
    const uint8_t *group = bytes + kHeader + first * kRecord;
    width = read32(group + 4);
    height = read32(group + 8);
    if (!width || !height || width > 16384 || height > 16384) return -EINVAL;
    size_t selected = first;
    uint32_t previous = 0;
    for (size_t i = first; i < count; ++i) {
        const uint8_t *r = bytes + kHeader + i * kRecord;
        if (read32(r + 4) != width || read32(r + 8) != height) break;
        const uint32_t current = read32(r);
        if (!current || current > INT32_MAX || current <= previous) return -EINVAL;
        previous = current;
        // This NCF revision is three-channel, four-scale, eight-bit; reject others.
        if (read32(r + 12) != 3 || read32(r + 16) != 4 ||
                read32(r + 20) != 8 || read32(r + 24) != 0) return -EINVAL;
        if (current <= uint32_t(iso)) selected = i;
    }
    const uint8_t *r = bytes + kHeader + selected * kRecord;
    profile.iso = read32(r);
    profile.width = width;
    profile.height = height;
    memcpy(profile.noise, r + 12, sizeof(profile.noise));
    return 0;
}

int32_t QCamera3HdrDenoise::process(const QCamera3HdrAlgo::Image &image,
        int32_t iso, bool sharpen, const std::atomic<bool> *cancelFlag,
        const char *path) {
    if (!QCamera3HdrAlgo::validImage(image) || iso <= 0) return -EINVAL;
    if (canceled(cancelFlag)) return -ECANCELED;
    Profile profile = {};
    int32_t rc = loadProfile(path, image, iso, profile);
    if (rc) return rc;
    const uint32_t stride = (image.width + 63) & ~63U;
    const uint64_t ySize = uint64_t(stride) * image.height;
    const uint64_t length = ySize + ySize / 2;
    // Bound memory in the 32-bit HAL independently of valid caller planes.
    if (length > 128U * 1024U * 1024U) return -EOVERFLOW;
    Buffer source{size_t(length)}, output{size_t(length)}, workspace{kWorkspace};
    if (!source.data || !output.data || !workspace.data) return -ENOMEM;
    ArcImage input = {0x802, int32_t(image.width), int32_t(image.height),
            {source.data, source.data + size_t(ySize), nullptr, nullptr},
            {int32_t(stride), int32_t(stride), 0, 0}};
    ArcImage result = input;
    result.planes[0] = output.data;
    result.planes[1] = output.data + size_t(ySize);
    memset(input.planes[1], 128, size_t(ySize / 2));
    memset(result.planes[1], 128, size_t(ySize / 2));
    copyRows(input.planes[0], stride, image.data + image.yOffset, image.yStride,
            image.width, image.height);
    copyRows(input.planes[1], stride, image.data + image.uvOffset, image.uvStride,
            image.width, image.height / 2);
    Lock lock;
    Api api;
    if (!api.valid()) return -ENOSYS;
    if (canceled(cancelFlag)) return -ECANCELED;
    void *manager = api.managerCreate(workspace.data, kWorkspace);
    if (!manager) return -ENOMEM;
    void *handle = nullptr;
    rc = api.create(manager, &handle);
    if (!rc && !handle) rc = -EIO;
    if (!rc) rc = api.type(handle, 1);
    if (!rc) rc = api.config(handle, profile.noise);
    // R11/R11s hdr_process constants: sharp=50, color=60, luminance=25.
    if (!rc) rc = api.luminance(handle, 25);
    if (!rc) rc = api.color(handle, 60);
    if (!rc) rc = api.sharp(handle, sharpen ? 50 : 0);
    if (!rc) rc = api.ring(handle, 0);
    if (!rc) rc = canceled(cancelFlag) ? -ECANCELED :
            api.denoise(handle, &input, &result, nullptr, nullptr);
    if (handle) {
        const int32_t cleanup = api.release(handle);
        if (!rc) rc = cleanup;
    }
    api.managerDestroy(manager);
    if (rc) return rc;
    if (canceled(cancelFlag)) return -ECANCELED;
    if (result.format != 0x802 || result.width != int32_t(image.width) ||
            result.height != int32_t(image.height) || result.planes[0] != output.data ||
            result.planes[1] != output.data + size_t(ySize) ||
            result.pitches[0] != int32_t(stride) || result.pitches[1] != int32_t(stride))
        return -EPROTO;
    copyRows(image.data + image.yOffset, image.yStride, result.planes[0], stride,
            image.width, image.height);
    copyRows(image.data + image.uvOffset, image.uvStride, result.planes[1], stride,
            image.width, image.height / 2);
    return 0;
}
}
