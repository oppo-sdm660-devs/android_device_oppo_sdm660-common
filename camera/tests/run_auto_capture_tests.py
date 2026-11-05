#!/usr/bin/env python3
"""Build and run the QCamera3AutoCapturePolicy host tests.

Pure C++ (no Android headers): verifies the luma histogram helpers and the
automatic HDR/night trigger decisions that the HAL3 metadata path feeds.
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
if os.environ.get("HOST_SANITIZE", "1") == "1":
    FLAGS += ["-fsanitize=address,undefined", "-fno-sanitize-recover=undefined"]


def main():
    out = Path(tempfile.mkdtemp(prefix="ocam-auto-capture-"))
    test = out / "auto_capture_policy_test"
    build = [CXX, *FLAGS, "-I", str(HAL3), str(HERE / "auto_capture_policy_test.cpp"),
             str(HAL3 / "QCamera3AutoCapturePolicy.cpp"), "-o", str(test)]
    print("+ " + " ".join(build))
    rc = subprocess.run(build).returncode
    if rc:
        return rc
    print("+ " + str(test))
    rc = subprocess.run([str(test)]).returncode
    print(f"artifacts: {out}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
