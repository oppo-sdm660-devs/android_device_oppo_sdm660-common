#!/usr/bin/env python3
"""Exercise the live HAL's accepted/native request ownership transitions."""
import os
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
HAL3 = HERE.parent / "QCamera2" / "HAL3"
out = Path(tempfile.mkdtemp(prefix="ocam-request-lifecycle-"))
flags = ["-std=gnu++17", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
         "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined"]
subprocess.run([os.environ.get("CXX", "g++"), *flags, "-I", str(HAL3),
                str(HERE / "request_lifecycle_test.cpp"), "-o", str(out / "test")], check=True)
subprocess.run([str(out / "test")], check=True)
print(f"artifacts: {out}")
