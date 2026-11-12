#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Verify the stock eye-detector wrapper used by QCamera3StockPicBest.

The wrapper owns all detector records. HALv3 passes packed NV12 source planes,
0x14c face blocks and the [frame][face] scores; no APICBEST replacement ABI is
constructed here. Evidence is checked against both device dumps independently.
"""
import argparse
import hashlib
import struct
from pathlib import Path

from hal1re import Image

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--stock-root", type=Path, default=Path("/home/cy/Project/R11/stock"))
options = parser.parse_args()
core_hash = "3ef68495cbf08a275112f4d3217e29a4c0443f9b10632e492afc2311217bafcd"
symbols = {
    "_Z15picbest_loadlibP20picbest_static_lib_t": (0x2db9c, 892),
    "_Z14picBestProcessP20picbest_static_lib_tiiiiPPhS2_iP20platform_face_data_tPA10_i": (0x2d738, 832),
    "_Z18picBestFusionImageP20picbest_static_lib_tiiiiPPhS2_iP20platform_face_data_tPA10_iii": (0x2c028, 4672),
    "_Z17picbest_unloadlibP20picbest_static_lib_t": (0x2df18, 46),
    "_Z21executeEyesOpenDetectP20picbest_static_lib_tP20platform_face_data_tiiiiPPhS4_PA10_iPA4_ii":
        (0x2bb64, 1124),
}
for device in ("R11", "R11s"):
    path = options.stock_root / device / "vendor/lib/libcamera_core_hwi.so"
    assert hashlib.sha256(path.read_bytes()).hexdigest() == core_hash, f"{device}: unknown core ABI"
    image = Image(str(path))
    for name, (address, length) in symbols.items():
        entry = image.find_sym(name)[0]
        assert entry["value"] & ~1 == address and entry["size"] == length, name
    # loadlib resolves the actual allocator/blink/picbest libraries and functions.
    for literal, instruction, expected in (
        (0x2de78, 0x2dbcc, b"/vendor/lib/libmpbase.so"),
        (0x2de8c, 0x2dc2c, b"/vendor/lib/libarcsoft_blink_detection_CoupleDis.so"),
        (0x2de90, 0x2dc42, b"ASBD_InitBlinkEngine"),
        (0x2de94, 0x2dc54, b"ASBD_BlinkDetect"),
        (0x2de98, 0x2dc66, b"ASBD_UninitBlinkEngine"),
        (0x2dea0, 0x2dc8a, b"/vendor/lib/libarcsoft_picbest.so"),
    ):
        offset, = struct.unpack("<I", image.read(literal, 4))
        assert image.read(instruction + 4 + offset, len(expected) + 1) == expected + b"\0"
    # Thumb-2 accesses face count +0x148, source W/H +0x140/+0x144,
    # builds NV12(0x802), and advances face blocks by 0x14c and score rows by 40.
    for address, opcode in (
        (0x2bbb8, "d6f84801"), (0x2bc46, "40f60200"),
        (0x2bed6, "08f5a678"), (0x2beca, "2834"),
        (0x2d954, "d7f84801"), (0x2d9a6, "0bf0c9f9"),
        (0x2c498, "40f6020e"),
    ):
        assert image.read(address, len(bytes.fromhex(opcode))) == bytes.fromhex(opcode), hex(address)
    print(f"{device}: stock eye-detector ABI PASS; core sha256={core_hash}")
