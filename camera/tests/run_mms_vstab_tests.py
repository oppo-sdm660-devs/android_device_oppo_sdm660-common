#!/usr/bin/env python3
"""Check the ARM32 MMS adapter against a controlled core, never the stock algorithm.

Real installed profiles are parsed and their geometry selects are checked. The
fake core exercises init/config/warp failures and the NV12/NV21 CPU renderer;
this is not device readiness, proprietary algorithm quality or throughput proof.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
HAL3 = HERE.parent / "QCamera2" / "HAL3"
VENDOR = HERE.parents[2] / "proprietary_vendor_oppo_sdm660-common" / "proprietary"
CXX = os.environ.get("CXX", "g++")
FLAGS = ["-m32", "-std=gnu++17", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
         "-fno-omit-frame-pointer"]
if os.environ.get("HOST_SANITIZE", "1") == "1":
    FLAGS += ["-fsanitize=address,undefined", "-fno-sanitize-recover=undefined"]


def run(command, env=None):
    print("+ " + " ".join(str(item) for item in command), flush=True)
    return subprocess.run([str(item) for item in command], env=env).returncode


def main():
    out = Path(tempfile.mkdtemp(prefix="ocam-mms-vstab-"))
    fake = out / "libmms_gyro_vstab.so"
    if run([CXX, *FLAGS, "-shared", "-fPIC", HERE / "fake_mms_core.cpp", "-o", fake]):
        return 1
    test = out / "mms_vstab_host_test"
    if run([CXX, *FLAGS, "-I", HAL3, "-I", HERE / "mms_host_stubs",
            HERE / "mms_vstab_host_test.cpp", HERE / "fake_mms_gl.cpp",
            HAL3 / "QCamera3MmsVstab.cpp", HAL3 / "QCamera3GyroVstab.cpp",
            HAL3 / "QCamera3HdrAlgo.cpp", "-ldl", "-lpthread", "-o", test]):
        return 1
    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = str(out) + os.pathsep + env.get("LD_LIBRARY_PATH", "")
    result = run([test, out, VENDOR], env=env)
    print(f"artifacts: {out}")
    return result


if __name__ == "__main__":
    sys.exit(main())
