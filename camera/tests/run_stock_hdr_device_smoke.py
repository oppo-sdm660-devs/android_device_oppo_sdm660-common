#!/usr/bin/env python3
"""Build a synthetic HDR/LLHDR adapter test; optionally run ARM32 vendor DSOs.

Only a unique /data/local/tmp folder is written. No camera, APK, HAL, or global
property is changed. Synthetic ISO/DRC values do not validate camera metadata.
"""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import uuid

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--ndk", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
parser.add_argument("--serial")
parser.add_argument("--width", type=int, default=640)
parser.add_argument("--height", type=int, default=480)
parser.add_argument("--algorithm", choices=["hdr", "llhdr", "both"], default="both")
args = parser.parse_args()
if args.serial and args.serial.startswith("emulator-"):
    parser.error("vendor DSO validation requires a physical device")
args.output.mkdir(parents=True, exist_ok=False)
here = Path(__file__).resolve().parent
hal3 = here.parent / "QCamera2/HAL3"
compiler = args.ndk / "toolchains/llvm/prebuilt/linux-x86_64/bin/armv7a-linux-androideabi36-clang++"
binary = args.output / "hdr-smoke32"
subprocess.run([str(compiler), "-std=c++11", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
                "-static-libstdc++", "-I", str(hal3), str(here / "stock_hdr_device_smoke.cpp"),
                str(hal3 / "QCamera3HdrAlgo.cpp"), str(hal3 / "QCamera3LowLightHdrAlgo.cpp"),
                "-ldl", "-o", str(binary)], check=True)
if not args.serial:
    print(binary)
    raise SystemExit(0)
adb = [os.environ.get("ADB", "adb"), "-s", args.serial]
remote = "/data/local/tmp/ocam-hdr-" + uuid.uuid4().hex[:12]
identity = subprocess.run([*adb, "shell", "getprop ro.product.device; getprop ro.build.version.sdk; getenforce"],
                          check=True, text=True, capture_output=True).stdout
hashes = subprocess.run([*adb, "shell", "sha256sum /vendor/lib/libarcsoft_high_dynamic_range.so "
                         "/vendor/lib/libarcsoft_low_light_hdr.so"],
                        check=True, text=True, capture_output=True).stdout
subprocess.run([*adb, "shell", "mkdir", "-p", remote], check=True)
subprocess.run([*adb, "push", str(binary), remote + "/hdr-smoke32"], check=True)
command = "chmod 755 " + shlex.quote(remote + "/hdr-smoke32") + "; " + \
    "LD_LIBRARY_PATH=/vendor/lib:/system/lib " + shlex.quote(remote + "/hdr-smoke32") + \
    " " + shlex.quote(remote) + f" {args.width} {args.height} " + args.algorithm
result = subprocess.run([*adb, "shell", command], text=True, capture_output=True)
(args.output / "device.log").write_text(identity + hashes + result.stdout + result.stderr)
summary = {"serial": args.serial, "identity": identity.splitlines(), "libraryHashes": hashes.splitlines(),
           "remoteDirectory": remote, "exitCode": result.returncode,
           "width": args.width, "height": args.height, "metrics": []}
for line in result.stdout.splitlines():
    if line.startswith("{"):
        summary["metrics"].append(json.loads(line))
(args.output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
subprocess.run([*adb, "pull", remote, str(args.output / "device")], check=True)
print(result.stdout, end="")
raise SystemExit(result.returncode)
