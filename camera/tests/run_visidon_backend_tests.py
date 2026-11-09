#!/usr/bin/env python3
"""Build and run the front Visidon panorama backend host tests (no device)."""

import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
HAL3 = HERE.parent / "QCamera2" / "HAL3"
CXX = os.environ.get("CXX", "g++")
FLAGS = ["-std=gnu++17", "-Wall", "-Wextra", "-Werror", "-O1", "-g"]
if os.environ.get("HOST_SANITIZE", "1") == "1":
    FLAGS += ["-fsanitize=address,undefined", "-fno-sanitize-recover=undefined"]


def run(cmd):
    print("+ " + " ".join(str(c) for c in cmd))
    return subprocess.run([str(c) for c in cmd]).returncode


def main():
    out = Path(tempfile.mkdtemp(prefix="ocam-visidon-"))
    test = out / "visidon_panorama_backend_test"
    build = [CXX, *FLAGS, "-I", HAL3, HERE / "visidon_panorama_backend_test.cpp",
             HAL3 / "visidon_jnienv.cpp", HAL3 / "QCamera3OppoVisidonPanorama.cpp",
             "-o", test, "-ldl"]
    if run(build):
        return 1
    rc = run([test])
    print(f"artifacts: {out}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
