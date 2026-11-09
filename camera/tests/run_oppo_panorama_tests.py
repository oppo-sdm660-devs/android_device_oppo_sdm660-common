#!/usr/bin/env python3
"""Build and run the OSS DocPanorama backend host tests (pure C++, no Android)."""

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
    out = Path(tempfile.mkdtemp(prefix="ocam-oppo-panorama-"))
    test = out / "oppo_panorama_backend_test"
    build = [CXX, *FLAGS, "-I", HAL3, HERE / "oppo_panorama_backend_test.cpp",
             HAL3 / "QCamera3OppoPanorama.cpp", "-o", test, "-lm"]
    if run(build):
        return 1
    rc = run([test, out])
    print(f"artifacts: {out}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
