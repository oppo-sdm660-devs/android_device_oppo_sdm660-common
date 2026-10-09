// SPDX-License-Identifier: Apache-2.0
//
// Compatibility shim for the stock Android-9 MMS/VStab libraries. They import
// the two-argument android::GraphicBuffer::lock(uint32_t, void**), which the
// Android-14 libui replaced with a four-argument overload. The object
// is created by the current libui, so forwarding through the current class
// layout and lockAsync path preserves the caller ABI (this pointer + usage +
// out pointer, status_t return).
#include <ui/GraphicBuffer.h>
#include <utils/Errors.h>

extern "C" int _ZN7android13GraphicBuffer4lockEjPPv(
        void* self, uint32_t inUsage, void** outPtr)
        asm("_ZN7android13GraphicBuffer4lockEjPPv");

extern "C" int _ZN7android13GraphicBuffer4lockEjPPv(
        void* self, uint32_t inUsage, void** outPtr) {
    if (self == nullptr) {
        return android::BAD_VALUE;
    }
    auto* buffer = static_cast<android::GraphicBuffer*>(self);
    return buffer->lock(inUsage, outPtr, nullptr, nullptr);
}
