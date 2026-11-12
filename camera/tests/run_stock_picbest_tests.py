#!/usr/bin/env python3
"""Exercise the stock eye-detector ABI, frame identity, and score selection under sanitizers."""
import os
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
HAL3 = HERE.parent / "QCamera2/HAL3"
FLAGS = [os.environ.get("CXX", "g++"), "-std=gnu++17", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
         "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-pthread", "-I", str(HAL3)]
with tempfile.TemporaryDirectory(prefix="ocam-picbest-") as folder:
    mock = Path(folder) / "libfakepicbest.so"
    test = Path(folder) / "stock_picbest_test"
    subprocess.run([*FLAGS, "-shared", "-fPIC", str(HERE / "fake_stock_picbest.cpp"), "-o", str(mock)], check=True)
    subprocess.run([*FLAGS, str(HERE / "stock_picbest_test.cpp"), str(HAL3 / "QCamera3StockPicBest.cpp"),
                    str(HAL3 / "QCamera3HdrAlgo.cpp"), "-ldl", "-o", str(test)], check=True)
    subprocess.run([str(test), str(mock)], check=True)
