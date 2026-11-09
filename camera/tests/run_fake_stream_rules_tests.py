#!/usr/bin/env python3
"""Host test for Android 16 Camera3FakeStream-only recognition."""
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
    out = Path(tempfile.mkdtemp(prefix="ocam-fake-stream-"))
    test = out / "fake_stream_rules_test"
    cmd = [CXX, *FLAGS, "-I", HAL3, HERE / "fake_stream_rules_test.cpp", "-o", test]
    print("+ " + " ".join(str(c) for c in cmd))
    if subprocess.run(cmd).returncode:
        return 1
    return subprocess.run([str(test)]).returncode


if __name__ == "__main__":
    sys.exit(main())
