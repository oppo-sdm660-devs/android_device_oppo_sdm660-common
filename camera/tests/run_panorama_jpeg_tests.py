#!/usr/bin/env python3
"""Build and run the QCamera3PanoramaJpeg host tests (libjpeg-turbo)."""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
HAL3 = HERE.parent / "QCamera2" / "HAL3"
CXX = os.environ.get("CXX", "g++")
FLAGS = ["-std=gnu++17", "-Wall", "-Wextra", "-Werror", "-O1", "-g", "-fsanitize=address,undefined",
         "-fno-sanitize-recover=undefined"]


def main():
    out = Path(tempfile.mkdtemp(prefix="ocam-panorama-jpeg-"))
    test = out / "panorama_jpeg_encoder_test"
    build = [CXX, *FLAGS, "-I", str(HAL3), str(HERE / "panorama_jpeg_encoder_test.cpp"),
             str(HAL3 / "QCamera3PanoramaJpeg.cpp"), "-ljpeg", "-o", str(test)]
    print("+ " + " ".join(build))
    if subprocess.run(build).returncode:
        return 1
    print("+ " + str(test))
    return subprocess.run([str(test)]).returncode


if __name__ == "__main__":
    sys.exit(main())
