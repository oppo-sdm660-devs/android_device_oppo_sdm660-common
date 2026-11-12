#!/usr/bin/env python3
"""Build and run the QCamera3FrontBeauty host tests against a fake ArcSoft DSO.

The fake DSO validates the adapter's evidenced call sequence and YV12 scratch
layout only; it never executes the proprietary algorithm. Builds with -m32 so the
ARM32 pointer sizes (and the 44-byte ASVLOFFSCREEN descriptor) match the device.
"""

import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
CAMERA = HERE.parent
HAL3 = CAMERA / "QCamera2" / "HAL3"
CXX = os.environ.get("CXX", "g++")

FLAGS = [
    "-m32", "-std=gnu++17", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
    "-fno-omit-frame-pointer",
]
SANITIZE = os.environ.get("HOST_SANITIZE", "1") == "1"
if SANITIZE:
    FLAGS += ["-fsanitize=address,undefined", "-fno-sanitize-recover=undefined"]


def run(cmd, env=None):
    print("+ " + " ".join(str(c) for c in cmd))
    proc = subprocess.run([str(c) for c in cmd], env=env)
    return proc.returncode


def main():
    out = Path(tempfile.mkdtemp(prefix="ocam-front-beauty-"))
    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = str(out) + os.pathsep + env.get("LD_LIBRARY_PATH", "")

    fake = out / "libarcsoft_beautyshot.so"
    rc = run([CXX, *FLAGS, "-shared", "-fPIC",
              HERE / "fake_arcsoft_beautyshot.cpp", "-o", fake])
    if rc:
        print("fake DSO build failed")
        return rc
    (out / "libarcsoft_relighting_pro_image.so").symlink_to(fake.name)

    test = out / "front_beauty_host_test"
    rc = run([CXX, *FLAGS, "-I", HAL3,
              HERE / "front_beauty_host_test.cpp",
              HAL3 / "QCamera3FrontBeauty.cpp",
              HAL3 / "QCamera3StockBeauty.cpp",
              HAL3 / "QCamera3HdrAlgo.cpp",
              "-o", test, "-ldl", "-lpthread"])
    if rc:
        print("host test build failed")
        return rc

    rc = run([test], env=env)
    print(f"artifacts: {out}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
