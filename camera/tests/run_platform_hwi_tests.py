#!/usr/bin/env python3
"""Build and run the OPPO platform HWI host tests.

Builds the fake liboppo_platform_hwi.so (records the reversed ABI calls) and the
host binary that links QCamera3OppoPlatformHwi.cpp + QCamera3OppoPlatformParams.cpp
against host stubs for cutils/properties.h and mm_camera_dbg.h.
"""

import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
HAL3 = HERE.parent / "QCamera2" / "HAL3"
CXX = os.environ.get("CXX", "g++")
FLAGS = ["-std=gnu++17", "-Wall", "-Wextra", "-Werror", "-O1", "-g"]
if os.environ.get("HOST_M32", "0") == "1":
    FLAGS += ["-m32"]
if os.environ.get("HOST_SANITIZE", "1") == "1":
    FLAGS += ["-fsanitize=address,undefined", "-fno-sanitize-recover=undefined"]


def run(cmd, env=None):
    print("+ " + " ".join(str(c) for c in cmd))
    return subprocess.run([str(c) for c in cmd], env=env).returncode


def main():
    out = Path(tempfile.mkdtemp(prefix="ocam-platform-hwi-"))
    fake = out / "liboppo_platform_hwi.so"
    if run([CXX, *FLAGS, "-shared", "-fPIC", "-I", HERE / "host_stubs",
            HERE / "fake_oppo_platform_hwi.cpp", "-o", fake]):
        return 1
    for name in ("libSCES.so", "libVDImageAdjustment.so", "libVDRTImageAdjustment.so"):
        (out / name).symlink_to(fake.name)
    test = out / "platform_hwi_host_test"
    build = [CXX, *FLAGS,
             "-I", HAL3,
             "-I", HERE, "-I", HERE / "host_stubs",
             HERE / "platform_hwi_host_test.cpp",
             HERE / "host_stubs" / "properties_stub.cpp",
             HERE / "host_stubs" / "fs_stub.cpp",
             HAL3 / "QCamera3OppoPlatformHwi.cpp",
             HAL3 / "QCamera3OppoPlatformParams.cpp",
             HAL3 / "QCamera3FrontBeauty.cpp",
             HAL3 / "QCamera3StockBeauty.cpp",
             HAL3 / "QCamera3HdrAlgo.cpp",
             "-ldl", "-lpthread", "-o", test]
    if run(build):
        return 1
    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = str(out) + os.pathsep + env.get("LD_LIBRARY_PATH", "")
    rc = run([test, fake], env=env)
    print(f"artifacts: {out}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
