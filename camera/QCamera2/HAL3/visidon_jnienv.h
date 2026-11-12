/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_VISIDON_JNIENV_H
#define QCAMERA3_VISIDON_JNIENV_H

#include <stddef.h>
#include <stdint.h>

namespace qcamera {

/*
 * Minimal fake JNIEnv for the stock Visidon selfie-panorama engine
 * (libVDSelfiePanorama.so, 32-bit, JNI-only interface).
 *
 * The stock library imports AndroidBitmap_* only for its Bitmap entry points
 * (nativeGetImage/nativeGuidanceImage); the frame -> process -> NV21/JPEG path
 * used here only touches a handful of JNIEnv slots (task-152 scan). No ART/JVM
 * is required.
 *
 * ABI note (verified on R11s after the task-152 smoke fix): a JNI entry does
 *     (*env)[slot]
 * so `env` must point at a variable holding the slot table pointer:
 *     void *slots[kSlotCount]; void **table = slots; void ***env = &table;
 * jniEnv() returns that pointer. Passing the table itself faults at slot
 * 184*4 = 0x2e0 on the first GetByteArrayElements call.
 *
 * Failure policy: an unexpected slot is never allowed to crash the HAL. It is
 * recorded via takeSlotError() and returns a neutral value (nullptr/0); the
 * backend checks the flag after every engine call and fails closed.
 */
class VisidonJniEnv {
public:
    static constexpr uint32_t kSlotCount = 240;
    static constexpr size_t kMaxArrayBytes = 16u * 1024u * 1024u;
    static constexpr uintptr_t kFakeThis = 1;

    enum Slot : uint32_t {
        kNewByteArray = 176,
        kNewIntArray = 179,
        kNewFloatArray = 181,
        kGetByteArrayElements = 184,
        kReleaseByteArrayElements = 192,
        kSetByteArrayRegion = 208,
        kSetIntArrayRegion = 211,
        kSetFloatArrayRegion = 213,
        kGetDirectBufferAddress = 230,
    };

    // Input wrappers handed to the engine in place of Java arrays/buffers.
    struct ByteArrayIn {
        const void *data = nullptr;
        size_t size = 0;
    };
    struct DirectBufferIn {
        void *addr = nullptr;
        size_t size = 0;
    };
    // Output wrappers returned by New*Array slots and filled by Set*Region.
    struct ByteArrayOut {
        uint8_t *data = nullptr;
        size_t capacity = 0;
        size_t length = 0;
    };
    struct IntArrayOut {
        int32_t *data = nullptr;
        size_t capacity = 0;
        size_t length = 0;
    };
    struct FloatArrayOut {
        float *data = nullptr;
        size_t capacity = 0;
        size_t length = 0;
    };

    VisidonJniEnv();
    ~VisidonJniEnv();
    VisidonJniEnv(const VisidonJniEnv &) = delete;
    VisidonJniEnv &operator=(const VisidonJniEnv &) = delete;

    // First argument for every Java_visidon_Lib_Panorama_* entry.
    void *jniEnv() { sActive = this; return &table_; }
    static void *fakeThis();

    // Output helpers are also used by host-test mock engines.
    ByteArrayOut *newByteArrayOut(size_t capacity);
    IntArrayOut *newIntArrayOut(size_t capacity);
    FloatArrayOut *newFloatArrayOut(size_t capacity);
    static void freeOutput(ByteArrayOut *out);
    static void freeOutput(IntArrayOut *out);
    static void freeOutput(FloatArrayOut *out);

    // True once a slot outside the supported subset was invoked.
    bool slotError() const { return slotError_; }
    bool takeSlotError() {
        const bool seen = slotError_;
        slotError_ = false;
        return seen;
    }

private:
    static VisidonJniEnv *active();
    static void *slotNewByteArray(void *env, int length);
    static void *slotNewIntArray(void *env, int length);
    static void *slotNewFloatArray(void *env, int length);
    static void *slotGetByteArrayElements(void *env, void *array, unsigned char *isCopy);
    static void slotReleaseByteArrayElements(void *env, void *array, void *elements, int mode);
    static void slotSetByteArrayRegion(void *env, void *array, int start, int length,
            const uint8_t *buffer);
    static void slotSetIntArrayRegion(void *env, void *array, int start, int length,
            const int32_t *buffer);
    static void slotSetFloatArrayRegion(void *env, void *array, int start, int length,
            const float *buffer);
    static void *slotGetDirectBufferAddress(void *env, void *buffer);
    static uintptr_t unsupportedSlot(void *env, ...);

    void reset();
    static thread_local VisidonJniEnv *sActive;
    void *slots_[kSlotCount];
    void **table_ = nullptr;
    bool slotError_ = false;
};

}  // namespace qcamera

#endif  // QCAMERA3_VISIDON_JNIENV_H
