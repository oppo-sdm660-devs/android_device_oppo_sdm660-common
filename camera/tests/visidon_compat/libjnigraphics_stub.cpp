// SPDX-License-Identifier: BSD-3-Clause
// Vendor stub replacing the system libjnigraphics.so for the 32-bit Visidon
// selfie-panorama engine (task-156). The engine's frame/JPEG path never calls
// these entry points; they exist only to satisfy DT_NEEDED while the vendor
// namespace cannot resolve /system/lib/libjnigraphics.so. Every call fails
// explicitly instead of pretending to access a Bitmap.
#include <stdint.h>
#include <stdio.h>

namespace {
void logUnsupportedOnce() {
    static bool logged = false;
    if (!logged) {
        logged = true;
        fprintf(stderr, "visidon: libjnigraphics stub installed; AndroidBitmap_* unsupported\n");
    }
}
}  // namespace

extern "C" {

// Mirror the AOSP AndroidBitmap_* signatures at the ABI level only (pointers /
// ints); the real types live in <android/bitmap.h> which vendor modules should
// not depend on.
int32_t AndroidBitmap_getInfo(void *env, void *jbitmap, void *info) {
    (void)env;
    (void)jbitmap;
    (void)info;
    logUnsupportedOnce();
    return -2; // ANDROID_BITMAP_RESULT_BAD_PARAMETER
}

int32_t AndroidBitmap_lockPixels(void *env, void *jbitmap, void **addrPtr) {
    (void)env;
    (void)jbitmap;
    if (addrPtr != nullptr) *addrPtr = nullptr;
    logUnsupportedOnce();
    return -2;
}

int32_t AndroidBitmap_unlockPixels(void *env, void *jbitmap) {
    (void)env;
    (void)jbitmap;
    logUnsupportedOnce();
    return -2;
}

}  // extern "C"
