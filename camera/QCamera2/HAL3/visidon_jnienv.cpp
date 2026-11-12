/* SPDX-License-Identifier: BSD-3-Clause */
#include "visidon_jnienv.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace qcamera {

thread_local VisidonJniEnv *VisidonJniEnv::sActive = nullptr;

VisidonJniEnv::VisidonJniEnv() { reset(); }

VisidonJniEnv::~VisidonJniEnv() {
    if (sActive == this) sActive = nullptr;
}

void VisidonJniEnv::reset() {
    for (uint32_t i = 0; i < kSlotCount; i++)
        slots_[i] = reinterpret_cast<void *>(&VisidonJniEnv::unsupportedSlot);
    slots_[kNewByteArray] = reinterpret_cast<void *>(&VisidonJniEnv::slotNewByteArray);
    slots_[kNewIntArray] = reinterpret_cast<void *>(&VisidonJniEnv::slotNewIntArray);
    slots_[kNewFloatArray] = reinterpret_cast<void *>(&VisidonJniEnv::slotNewFloatArray);
    slots_[kGetByteArrayElements] =
            reinterpret_cast<void *>(&VisidonJniEnv::slotGetByteArrayElements);
    slots_[kReleaseByteArrayElements] =
            reinterpret_cast<void *>(&VisidonJniEnv::slotReleaseByteArrayElements);
    slots_[kSetByteArrayRegion] =
            reinterpret_cast<void *>(&VisidonJniEnv::slotSetByteArrayRegion);
    slots_[kSetIntArrayRegion] =
            reinterpret_cast<void *>(&VisidonJniEnv::slotSetIntArrayRegion);
    slots_[kSetFloatArrayRegion] =
            reinterpret_cast<void *>(&VisidonJniEnv::slotSetFloatArrayRegion);
    slots_[kGetDirectBufferAddress] =
            reinterpret_cast<void *>(&VisidonJniEnv::slotGetDirectBufferAddress);
    table_ = slots_;
    slotError_ = false;
    sActive = this;
}

VisidonJniEnv *VisidonJniEnv::active() {
    if (sActive == nullptr) {
        std::fprintf(stderr, "visidon: fake JNIEnv used without an owner\n");
        std::abort();
    }
    return sActive;
}

void *VisidonJniEnv::fakeThis() {
    return reinterpret_cast<void *>(kFakeThis);
}

VisidonJniEnv::ByteArrayOut *VisidonJniEnv::newByteArrayOut(size_t capacity) {
    if (capacity == 0 || capacity > kMaxArrayBytes) return nullptr;
    auto *out = static_cast<ByteArrayOut *>(std::calloc(1, sizeof(ByteArrayOut)));
    if (out == nullptr) return nullptr;
    out->data = static_cast<uint8_t *>(std::calloc(capacity, 1));
    if (out->data == nullptr) {
        std::free(out);
        return nullptr;
    }
    out->capacity = capacity;
    return out;
}

VisidonJniEnv::IntArrayOut *VisidonJniEnv::newIntArrayOut(size_t capacity) {
    if (capacity == 0 || capacity > kMaxArrayBytes / sizeof(int32_t)) return nullptr;
    auto *out = static_cast<IntArrayOut *>(std::calloc(1, sizeof(IntArrayOut)));
    if (out == nullptr) return nullptr;
    out->data = static_cast<int32_t *>(std::calloc(capacity, sizeof(int32_t)));
    if (out->data == nullptr) {
        std::free(out);
        return nullptr;
    }
    out->capacity = capacity;
    return out;
}

VisidonJniEnv::FloatArrayOut *VisidonJniEnv::newFloatArrayOut(size_t capacity) {
    if (capacity == 0 || capacity > kMaxArrayBytes / sizeof(float)) return nullptr;
    auto *out = static_cast<FloatArrayOut *>(std::calloc(1, sizeof(FloatArrayOut)));
    if (out == nullptr) return nullptr;
    out->data = static_cast<float *>(std::calloc(capacity, sizeof(float)));
    if (out->data == nullptr) {
        std::free(out);
        return nullptr;
    }
    out->capacity = capacity;
    return out;
}

void VisidonJniEnv::freeOutput(ByteArrayOut *out) {
    if (out == nullptr) return;
    std::free(out->data);
    std::free(out);
}

void VisidonJniEnv::freeOutput(IntArrayOut *out) {
    if (out == nullptr) return;
    std::free(out->data);
    std::free(out);
}

void VisidonJniEnv::freeOutput(FloatArrayOut *out) {
    if (out == nullptr) return;
    std::free(out->data);
    std::free(out);
}

void *VisidonJniEnv::slotNewByteArray(void *env, int length) {
    (void)env;
    if (length <= 0) return nullptr;
    return active()->newByteArrayOut(size_t(length));
}

void *VisidonJniEnv::slotNewIntArray(void *env, int length) {
    (void)env;
    if (length <= 0) return nullptr;
    return active()->newIntArrayOut(size_t(length));
}

void *VisidonJniEnv::slotNewFloatArray(void *env, int length) {
    (void)env;
    if (length <= 0) return nullptr;
    return active()->newFloatArrayOut(size_t(length));
}

void *VisidonJniEnv::slotGetByteArrayElements(void *env, void *array, unsigned char *isCopy) {
    (void)env;
    if (isCopy != nullptr) *isCopy = 0;
    if (array == nullptr) return nullptr;
    return const_cast<void *>(static_cast<ByteArrayIn *>(array)->data);
}

void VisidonJniEnv::slotReleaseByteArrayElements(void *env, void *array,
        void *elements, int mode) {
    (void)env;
    (void)array;
    (void)elements;
    (void)mode;
}

void VisidonJniEnv::slotSetByteArrayRegion(void *env, void *array, int start,
        int length, const uint8_t *buffer) {
    (void)env;
    auto *out = static_cast<ByteArrayOut *>(array);
    if (out == nullptr || out->data == nullptr || buffer == nullptr ||
            start < 0 || length < 0 || size_t(start) > out->length ||
            size_t(start) > out->capacity || size_t(length) > out->capacity - size_t(start)) {
        active()->slotError_ = true;
        return;
    }
    std::memcpy(out->data + start, buffer, size_t(length));
    out->length = std::max(out->length, size_t(start) + size_t(length));
}

void VisidonJniEnv::slotSetIntArrayRegion(void *env, void *array, int start,
        int length, const int32_t *buffer) {
    (void)env;
    auto *out = static_cast<IntArrayOut *>(array);
    if (out == nullptr || out->data == nullptr || buffer == nullptr ||
            start < 0 || length < 0 || size_t(start) > out->length ||
            size_t(start) > out->capacity || size_t(length) > out->capacity - size_t(start)) {
        active()->slotError_ = true;
        return;
    }
    std::memcpy(out->data + start, buffer, size_t(length) * sizeof(int32_t));
    out->length = std::max(out->length, size_t(start) + size_t(length));
}

void VisidonJniEnv::slotSetFloatArrayRegion(void *env, void *array, int start,
        int length, const float *buffer) {
    (void)env;
    auto *out = static_cast<FloatArrayOut *>(array);
    if (out == nullptr || out->data == nullptr || buffer == nullptr ||
            start < 0 || length < 0 || size_t(start) > out->length ||
            size_t(start) > out->capacity || size_t(length) > out->capacity - size_t(start)) {
        active()->slotError_ = true;
        return;
    }
    std::memcpy(out->data + start, buffer, size_t(length) * sizeof(float));
    out->length = std::max(out->length, size_t(start) + size_t(length));
}

void *VisidonJniEnv::slotGetDirectBufferAddress(void *env, void *buffer) {
    (void)env;
    if (buffer == nullptr) return nullptr;
    return static_cast<DirectBufferIn *>(buffer)->addr;
}

uintptr_t VisidonJniEnv::unsupportedSlot(void *env, ...) {
    (void)env;
    if (sActive != nullptr) sActive->slotError_ = true;
    return 0;
}

}  // namespace qcamera
