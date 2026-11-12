#!/usr/bin/env python3
"""Build/run dual geometry + source/reference host tests."""
import os, subprocess, sys, tempfile
from pathlib import Path
HERE = Path(__file__).resolve().parent
HAL3 = HERE.parent / "QCamera2" / "HAL3"
CXX = os.environ.get("CXX", "g++")
FLAGS = ["-std=gnu++17", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
         "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
         "-pthread", "-I", str(HAL3)]
SOURCES = ["QCamera3DualGeometry.cpp", "QCamera3DualCapture.cpp",
           "QCamera3DualBokehAlgo.cpp", "QCamera3HdrAlgo.cpp"]
def main():
    out = Path(tempfile.mkdtemp(prefix="ocam-dual-geometry-"))
    test = out / "dual_geometry_test"
    library = out / "libarcsoft_dualcam_refocus.so"
    command = [CXX, *FLAGS, "-shared", "-fPIC", str(HERE / "fake_arcsoft_dualcam.cpp"),
               "-o", str(library)]
    print("+ " + " ".join(command))
    if subprocess.run(command).returncode: return 1
    command = [CXX, *FLAGS, str(HERE / "dual_geometry_test.cpp"),
               *[str(HAL3 / name) for name in SOURCES],
               "-o", str(test), "-lcrypto", "-ldl"]
    print("+ " + " ".join(command))
    if subprocess.run(command).returncode: return 1
    print("+ " + str(test))
    environment = os.environ.copy()
    environment["LD_LIBRARY_PATH"] = str(out)
    return subprocess.run([str(test)], env=environment).returncode
if __name__ == "__main__": sys.exit(main())
