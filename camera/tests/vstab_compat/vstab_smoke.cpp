// SPDX-License-Identifier: Apache-2.0
#include <dlfcn.h>
#include <initializer_list>
#include <string.h>
#include <stdio.h>

namespace {
struct Lib {
    const char* name;
    const char* path;
};

const Lib kLibs[] = {
    {"ui_shim", "/vendor/lib/libui_vstab_shim.so"},
    {"mms_gyro", "/vendor/lib/libmms_gyro_vstab.so"},
    {"mms_gyro_auth", "/vendor/lib/libmms_gyro_vstab_auth.so"},
    {"mms_warper", "/vendor/lib/libmms_warper_vstab.so"},
    {"mms_hal", "/vendor/lib/libmms_hal_vstab.so"},
};
}  // namespace

int main(int argc, char** argv) {
    int failures = 0;
    for (const Lib& lib : kLibs) {
        void* handle = dlopen(lib.path, RTLD_NOW | RTLD_GLOBAL);
        if (handle == nullptr) {
            printf("FAILED %s: %s\n", lib.name, dlerror());
            ++failures;
        } else {
            printf("LOADED %s\n", lib.name);
        }
    }
    if (failures == 0) {
        void* hal = dlopen(kLibs[4].path, RTLD_NOW | RTLD_NOLOAD);
        for (const char* symbol : {"VStabInitialize", "VStabDeinitialize",
                                   "VStabProcessFrame", "VStabProcessGyro"}) {
            void* fn = hal == nullptr ? nullptr : dlsym(hal, symbol);
            printf("%s %s\n", fn != nullptr ? "FOUND" : "MISSING", symbol);
            if (fn == nullptr) ++failures;
        }
        if (failures == 0 && argc > 1 && strcmp(argv[1], "--init") == 0) {
            using InitFn = int (*)(int, void*);
            using DeinitFn = int (*)(int);
            auto init = reinterpret_cast<InitFn>(dlsym(hal, "VStabInitialize"));
            auto deinit = reinterpret_cast<DeinitFn>(dlsym(hal, "VStabDeinitialize"));
            printf("CALL VStabInitialize(0,nullptr)=%d\n", init(0, nullptr));
            printf("CALL VStabDeinitialize(0)=%d\n", deinit(0));
        }
    }
    printf("RESULT %s failures=%d\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
