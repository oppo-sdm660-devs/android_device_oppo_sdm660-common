#!/usr/bin/env python3
"""Compile production HAL objects using ARM32 NDK and real Android platform headers.

This checks translation units, not Soong dependency resolution or a linked ROM. The Android
tree must already contain generated displayservice and HIDL headers. No Android
tree, kernel tree, or build configuration is modified.
"""

import concurrent.futures
import hashlib
import json
import re
import argparse
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--android-root", type=Path, required=True)
    parser.add_argument("--ndk", type=Path, required=True)
    parser.add_argument("--kernel-uapi", type=Path, required=True)
    parser.add_argument("--api", type=int, default=35)
    parser.add_argument("--output", type=Path, required=True, help="Object, diagnostics and exact command directory")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("sources", nargs="*", help="Paths relative to camera/; defaults to camera.sdm660 sources")
    args = parser.parse_args()
    camera = Path(__file__).resolve().parent.parent
    android = args.android_root.resolve()
    compiler_dir = args.ndk.resolve() / "toolchains/llvm/prebuilt/linux-x86_64/bin"
    local_includes = [
        "QCamera2", "QCamera2/HAL", "QCamera2/HAL3", "QCamera2/util",
        "QCamera2/stack/common", "QCamera2/stack/common/leak",
        "QCamera2/stack/mm-camera-interface/inc",
        "QCamera2/stack/mm-jpeg-interface/inc",
        "mm-image-codec/qexif", "mm-image-codec/qomx_core",
    ]
    platform_includes = [
        "system/core/libcutils/include", "system/core/libutils/include",
        "system/logging/liblog/include", "system/core/libsystem/include",
        "system/libbase/include", "hardware/libhardware/include",
        "system/media/camera/include", "external/boringssl/src/include", "external/libjpeg-turbo",
        "hardware/interfaces/camera/common/default/include",
        "frameworks/native/include", "frameworks/native/libs/nativebase/include",
        "frameworks/native/libs/math/include",
        "frameworks/native/libs/nativewindow/include", "frameworks/native/libs/ui/include",
        "frameworks/native/libs/binder/include", "frameworks/native/libs/gui/include",
        "frameworks/native/libs/binder/ndk/include_ndk",
        "frameworks/native/libs/binder/ndk/include_platform",
        "frameworks/native/libs/binder/ndk/include_cpp",
        "system/core/libsync/include", "system/libhidl/base/include",
        "system/libhidl/transport/include", "system/libfmq/base", "system/libfmq/include",
        "hardware/qcom-caf/msm8998/display/gralloc",
        "hardware/qcom-caf/msm8998/display/libqdutils",
        "hardware/qcom-caf/msm8998/display/libqservice",
        "hardware/qcom-caf/msm8998/display/include",
        "hardware/qcom-caf/msm8998/display/sdm/include",
        "frameworks/native/headers/media_plugin",
        "frameworks/native/headers/media_plugin/media/openmax",
    ]
    includes = [camera / path for path in local_includes]
    includes += [android / path for path in platform_includes]
    includes += [args.kernel_uapi.resolve()]
    for root in ["frameworks/hardware/interfaces/displayservice", "system/libhidl/transport",
                 "frameworks/hardware/interfaces/sensorservice", "hardware/interfaces/sensors",
                 "hardware/interfaces/graphics"]:
        generated = android / "out/soong/.intermediates" / root
        includes += sorted(generated.glob("**/*genc++_headers/gen"))
        includes += sorted(generated.glob("**/*-ndk-source/gen/include"))
    definitions = [
        "__ANDROID_VNDK__", "__ANDROID_VENDOR__", "__EXPORTED_HEADERS__",
        "SYSTEM_HEADER_PREFIX=sys", "VENUS_PRESENT", "UBWC_PRESENT", "_ANDROID_",
        "_ANDROID", "QCAMERA_HAL3_SUPPORT", "SUPPORT_ONLY_HAL3", "HAL3",
        "HAS_MULTIMEDIA_HINTS", "USE_CAMERA_METABUFFER_UTILS", "USE_DISPLAY_SERVICE",
        "USE_VENDOR_PROP", "VIDEO_EXPLICIT_UBWC", "SUPPORT_POWER_HINT_XML",
        "QCAMERA_REDEFINE_LOG", "FDLEAK_FLAG", "MEMLEAK_FLAG",
        "USE_KERNEL_VERSION_GE_4_4_DEFS",
    ]
    blocks = re.findall(r'cc_library_shared\s*\{(.*?)\n\}', (camera / "Android.bp").read_text(), re.S)
    module = next(block for block in blocks if 'name: "camera.sdm660"' in block)
    sources = args.sources or re.findall(r'"([^"]+\.(?:cpp|c))"',
            re.search(r'srcs:\s*\[(.*?)\]', module, re.S)[1])
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    def check(source):
        path = camera / source
        source_hash = hashlib.sha256(path.read_bytes()).hexdigest()
        cxx = path.suffix != ".c"
        compiler = compiler_dir / (
            f"armv7a-linux-androideabi{args.api}-clang" + ("++" if cxx else ""))
        obj = out / (source.replace('/', '__') + '.o')
        command = [str(compiler), "-std=gnu++17" if cxx else "-std=gnu11",
                   "-fPIC", "-O1", "-Wno-compound-token-split-by-macro"]
        if cxx:
            command += ["-fno-exceptions", "-fno-rtti", "-nostdinc++", "-isystem",
                    str(android / 'prebuilts/clang/host/linux-x86/clang-r536225/include/c++/v1')]
        command += [f"-D{definition}" for definition in definitions]
        command += [item for directory in includes for item in ("-I", str(directory))]
        command += ["-c", str(path), "-o", str(obj)]
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        obj.with_suffix(obj.suffix + '.command.json').write_text(json.dumps(command, indent=2) + '\n')
        obj.with_suffix(obj.suffix + '.log').write_text(result.stdout)
        stable = source_hash == hashlib.sha256(path.read_bytes()).hexdigest()
        return {"source": source, "source_sha256": source_hash,
                "status": result.returncode if stable else 1, "source_stable": stable,
                "log": str(obj.with_suffix(obj.suffix + '.log'))}
    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as executor:
        for result in executor.map(check, sources):
            results.append(result)
            print(f"{'PASS' if result['status'] == 0 else 'FAIL'} ARM32 object: {result['source']}", flush=True)
            if result['status']:
                print(Path(result['log']).read_text()[:6000], flush=True)
    (out / 'summary.json').write_text(json.dumps(results, indent=2) + '\n')
    raise SystemExit(any(result['status'] for result in results))


if __name__ == "__main__":
    main()
