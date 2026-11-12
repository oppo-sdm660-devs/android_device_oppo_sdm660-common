/* SPDX-License-Identifier: BSD-3-Clause */
// Device-only ABI probe. Run outside the provider to isolate a vendor warp fault.
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {
constexpr int kSlots = 240;
struct Input { const void *data; size_t length; };
struct Output { void *data; size_t capacity; size_t length; };
void *slots[kSlots];
void *table = slots;

void unsupported() {
    fprintf(stderr, "unexpected JNI slot\n");
    abort();
}
void *newArray(int length, size_t elementBytes) {
    if (length <= 0 || size_t(length) * elementBytes > 16 * 1024 * 1024) return nullptr;
    auto *out = static_cast<Output *>(calloc(1, sizeof(Output)));
    if (out == nullptr) return nullptr;
    out->data = calloc(size_t(length), elementBytes);
    out->capacity = size_t(length);
    return out;
}
void *newBytes(void *, int length) { return newArray(length, 1); }
void *newInts(void *, int length) { return newArray(length, sizeof(int32_t)); }
void *newFloats(void *, int length) { return newArray(length, sizeof(float)); }
void setRegion(void *array, int start, int length, const void *data, size_t elementBytes) {
    auto *out = static_cast<Output *>(array);
    if (out == nullptr || data == nullptr || start < 0 || length < 0 ||
            size_t(start) + size_t(length) > out->capacity) abort();
    memcpy(static_cast<uint8_t *>(out->data) + size_t(start) * elementBytes,
            data, size_t(length) * elementBytes);
    out->length = size_t(start) + size_t(length);
}
void setBytes(void *, void *array, int start, int length, const void *data) {
    setRegion(array, start, length, data, 1);
}
void setInts(void *, void *array, int start, int length, const void *data) {
    setRegion(array, start, length, data, sizeof(int32_t));
}
void setFloats(void *, void *array, int start, int length, const void *data) {
    setRegion(array, start, length, data, sizeof(float));
}
void *getBytes(void *, void *array, unsigned char *copy) {
    if (copy != nullptr) *copy = 0;
    return const_cast<void *>(static_cast<Input *>(array)->data);
}
void releaseBytes(void *, void *, void *, int) {}
void freeArray(Output *array) {
    if (array != nullptr) { free(array->data); free(array); }
}
using Init = long long (*)(void *, void *, int, int, int, int);
using Add = int (*)(void *, void *, void *, int, int, int, long long);
using Measure = void *(*)(void *, void *, void *, int, int, float, float, long long);
using Process = void *(*)(void *, void *, long long);
using Release = void (*)(void *, void *, long long);
}

int main(int argc, char **argv) {
    setbuf(stdout, nullptr);
    int width = 1440, height = 1080, focal = 1102, transposed = 0, frames = 5;
    int processAfterAdd = 0, delayMs = 250;
    const char *mode = "add-measure";
    const char *library = "/vendor/lib/libVDSelfiePanorama.so";
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--width")) width = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--height")) height = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--focal")) focal = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--transpose")) transposed = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--frames")) frames = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--process")) processAfterAdd = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--delay-ms")) delayMs = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--mode")) mode = argv[i + 1];
        else if (!strcmp(argv[i], "--library")) library = argv[i + 1];
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    if (width < 16 || height < 16 || width > 4096 || height > 4096 ||
            (width & 1) || (height & 1) || focal <= 0 || frames < 1 || frames > 30 ||
            delayMs < 0 || delayMs > 5000 ||
            (strcmp(mode, "measure") && strcmp(mode, "add-measure") && strcmp(mode, "cycle"))) return 2;
    for (auto &slot : slots) slot = reinterpret_cast<void *>(unsupported);
    slots[176] = reinterpret_cast<void *>(newBytes);
    slots[179] = reinterpret_cast<void *>(newInts);
    slots[181] = reinterpret_cast<void *>(newFloats);
    slots[184] = reinterpret_cast<void *>(getBytes);
    slots[192] = reinterpret_cast<void *>(releaseBytes);
    slots[208] = reinterpret_cast<void *>(setBytes);
    slots[211] = reinterpret_cast<void *>(setInts);
    slots[213] = reinterpret_cast<void *>(setFloats);
    void *lib = dlopen(library, RTLD_NOW | RTLD_GLOBAL);
    if (lib == nullptr) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 3; }
    const auto init = reinterpret_cast<Init>(dlsym(lib, "Java_visidon_Lib_Panorama_nativeInit"));
    const auto add = reinterpret_cast<Add>(dlsym(lib, "Java_visidon_Lib_Panorama_nativeAddYUVFrame"));
    const auto measure = reinterpret_cast<Measure>(dlsym(lib, "Java_visidon_Lib_Panorama_nativeMeasureOverlap"));
    const auto process = reinterpret_cast<Process>(dlsym(lib, "Java_visidon_Lib_Panorama_nativeProcess"));
    const auto release = reinterpret_cast<Release>(dlsym(lib, "Java_visidon_Lib_Panorama_nativeRelease"));
    if (!init || !add || !measure || !process || !release) return 4;
    const size_t bytes = size_t(width) * height * 3 / 2;
    const size_t page = size_t(sysconf(_SC_PAGESIZE));
    const size_t aligned = (bytes + page - 1) / page * page;
    auto *mapping = static_cast<uint8_t *>(mmap(nullptr, aligned + 2 * page,
            PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0));
    if (mapping == MAP_FAILED || mprotect(mapping, page, PROT_NONE) ||
            mprotect(mapping + page + aligned, page, PROT_NONE)) return 5;
    uint8_t *pixels = mapping + page + aligned - bytes;
    for (int y = 0; y < height; y++) for (int x = 0; x < width; x++)
        pixels[size_t(y) * width + x] = uint8_t((x * 7 + y * 13 + (x / 17) * 31) & 255);
    memset(pixels + size_t(width) * height, 128, bytes - size_t(width) * height);
    Input input{pixels, bytes};
    void *env = &table;
    void *thiz = reinterpret_cast<void *>(1);
    printf("config=%dx%d focal=%d transpose=%d frames=%d mode=%s process=%d guard=%p\n",
            width, height, focal, transposed, frames, mode, processAfterAdd,
            pixels + bytes);
    const long long engine = init(env, thiz, width, height, focal, transposed);
    printf("init=%lld\n", engine);
    if (engine == 0) return 6;
    auto addAndWait = [&](int displacement) {
        printf("add start displacement=%d\n", displacement);
        const int status = add(env, thiz, &input, width, height, displacement, engine);
        printf("add status=%d\n", status);
        if (processAfterAdd) {
            printf("process start\n");
            auto *dims = static_cast<Output *>(process(env, thiz, engine));
            printf("process dims=%d,%d\n", dims && dims->length >= 2 ? static_cast<int32_t *>(dims->data)[0] : -1,
                    dims && dims->length >= 2 ? static_cast<int32_t *>(dims->data)[1] : -1);
            freeArray(dims);
        }
        if (delayMs) usleep(useconds_t(delayMs) * 1000);
    };
    if (!strcmp(mode, "add-measure")) addAndWait(0);
    float x = 0, y = 0;
    for (int i = 0; i < frames; i++) {
        printf("measure#%d start input=%p bytes=%zu prior=%g,%g\n", i, pixels, bytes, x, y);
        auto *position = static_cast<Output *>(measure(env, thiz, &input, width, height, x, y, engine));
        if (position == nullptr || position->length != 4) { fprintf(stderr, "invalid measure output\n"); return 7; }
        auto *values = static_cast<float *>(position->data);
        x = values[0]; y = values[1];
        printf("measure#%d result=%g,%g quality=%g frames=%g\n", i, x, y, values[2], values[3]);
        freeArray(position);
        if (!strcmp(mode, "cycle")) addAndWait(transposed ? int(y * height) : int(x * width));
        else if (delayMs) usleep(useconds_t(delayMs) * 1000);
    }
    printf("release start\n");
    release(env, thiz, engine);
    printf("PASS release complete\n");
    munmap(mapping, aligned + 2 * page);
    dlclose(lib);
    return 0;
}
