#!/usr/bin/env python3
"""Build/run an ARM32 synthetic-input test using device vendor libraries.

Writes only a unique /data/local/tmp directory; does not install APK/HAL or change properties.
"""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import uuid

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--serial", required=True)
parser.add_argument("--ndk", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
parser.add_argument("--width", type=int, default=640)
parser.add_argument("--height", type=int, default=480)
args = parser.parse_args()
if args.serial.startswith("emulator-"):
    parser.error("this validation requires a physical device")
args.output.mkdir(parents=True, exist_ok=False)
here = Path(__file__).resolve().parent
hal3 = here.parent / "QCamera2/HAL3"
compiler = args.ndk / "toolchains/llvm/prebuilt/linux-x86_64/bin/armv7a-linux-androideabi36-clang++"
binary = args.output / "superzoom-smoke32"
subprocess.run([str(compiler), "-std=c++17", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
                "-static-libstdc++", "-I", str(hal3), str(here / "stock_superphoto_device_smoke.cpp"),
                str(hal3 / "QCamera3StockSuperPhoto.cpp"), str(hal3 / "QCamera3HdrAlgo.cpp"),
                "-ldl", "-o", str(binary)], check=True)
adb = [os.environ.get("ADB", "adb"), "-s", args.serial]
remote = "/data/local/tmp/ocam-superzoom-" + uuid.uuid4().hex[:12]
identity = subprocess.run([*adb, "shell", "getprop ro.product.device; getprop ro.build.version.sdk; getenforce"],
                          check=True, text=True, capture_output=True).stdout
subprocess.run([*adb, "shell", "mkdir", "-p", remote], check=True)
subprocess.run([*adb, "push", str(binary), remote + "/superzoom-smoke32"], check=True)
command = "chmod 755 " + shlex.quote(remote + "/superzoom-smoke32") + "; " + \
    "LD_LIBRARY_PATH=/vendor/lib:/system/lib " + shlex.quote(remote + "/superzoom-smoke32") + \
    " " + shlex.quote(remote) + f" {args.width} {args.height}"
result = subprocess.run([*adb, "shell", command], text=True, capture_output=True)
(args.output / "device.log").write_text(identity + result.stdout + result.stderr)
summary = {"serial": args.serial, "identity": identity.splitlines(), "remoteDirectory": remote,
           "exitCode": result.returncode, "width": args.width, "height": args.height}
for line in result.stdout.splitlines():
    if line.startswith("{"):
        summary["metrics"] = json.loads(line)
(args.output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
subprocess.run([*adb, "pull", remote, str(args.output / "device")], check=True)
print(result.stdout, end="")
raise SystemExit(result.returncode)
