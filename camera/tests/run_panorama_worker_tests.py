#!/usr/bin/env python3
"""Exercise bounded panorama copying, queue, completion and cancellation with ASan/UBSan."""
import os
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
HAL3 = HERE.parent / "QCamera2/HAL3"
with tempfile.TemporaryDirectory(prefix="ocam-panorama-worker-") as output:
    binary = Path(output) / "panorama_worker_test"
    command = [os.environ.get("CXX", "g++"), "-std=gnu++17", "-Wall", "-Wextra", "-Werror",
               "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
               "-pthread", "-I", str(HAL3), str(HERE / "panorama_worker_test.cpp"),
               *[str(HAL3 / name) for name in ("QCamera3PanoramaWorker.cpp", "QCamera3OppoPanorama.cpp",
                                               "QCamera3PanoramaJpeg.cpp")],
               "-ljpeg", "-o", str(binary)]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)
