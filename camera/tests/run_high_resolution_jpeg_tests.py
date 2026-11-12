#!/usr/bin/env python3
"""Check the production high-resolution JPEG encoder, including 9216x6912 SOF."""
import os
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
HAL3 = HERE.parent / "QCamera2/HAL3"
with tempfile.TemporaryDirectory(prefix="ocam-highres-jpeg-") as folder:
    executable = Path(folder) / "high_resolution_jpeg_test"
    subprocess.run([os.environ.get("CXX", "g++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                    "-I", str(HAL3), str(HERE / "high_resolution_jpeg_test.cpp"),
                    str(HAL3 / "QCamera3PanoramaJpeg.cpp"), str(HAL3 / "QCamera3HdrAlgo.cpp"),
                    "-ljpeg", "-ldl", "-pthread", "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
