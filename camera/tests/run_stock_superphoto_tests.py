#!/usr/bin/env python3
"""Check the ARM32 direct-adapter ABI, full-FOV precondition and atomic commit.

The fixture validates inputs/calls, not proprietary alignment or super-resolution.
"""
import os
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
HAL3 = HERE.parent / "QCamera2/HAL3"
FLAGS = ["-m32", "-std=gnu++17", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
         "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined"]
with tempfile.TemporaryDirectory(prefix="ocam-superphoto-") as directory:
    out = Path(directory)
    compiler = os.environ.get("CXX", "g++")
    fixture = out / "libVDSuperPhotoAPI.so"
    subprocess.run([compiler, *FLAGS, "-shared", "-fPIC", str(HERE / "stock_superphoto_fixture.cpp"),
                    "-o", str(fixture)], check=True)
    (out / "libcamera_core_hwi.so").symlink_to(fixture.name)
    binary = out / "stock_superphoto_test"
    subprocess.run([compiler, *FLAGS, "-I", str(HAL3), str(HERE / "stock_superphoto_adapter_test.cpp"),
                    str(HAL3 / "QCamera3StockSuperPhoto.cpp"), str(HAL3 / "QCamera3HdrAlgo.cpp"),
                    "-L", str(out), "-lVDSuperPhotoAPI", "-ldl", "-pthread", "-o", str(binary)], check=True)
    env = dict(os.environ, LD_LIBRARY_PATH=str(out))
    subprocess.run([str(binary)], env=env, check=True)
