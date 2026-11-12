#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Verify the R11/R11s beauty table and pixel-stage ABI in read-only stock files.

Usage: front_beauty_profile.py --vendor-lib-dir PATH [--vendor-lib-dir PATH]
"""
import argparse
import hashlib
from pathlib import Path

from hal1re import Disassembler, Image

CORE_SHA256 = "3ef68495cbf08a275112f4d3217e29a4c0443f9b10632e492afc2311217bafcd"
SOFTEN = [0, 10, 30, 50, 60, 70, 80]


def instructions(image, start, length):
    return {row["addr"]: (row["mnemonic"], row["op_str"])
            for row in Disassembler(image).disasm(start, length)}


def verify(directory):
    core_path = directory / "libcamera_core_hwi.so"
    platform_path = directory / "liboppo_platform_hwi.so"
    assert hashlib.sha256(core_path.read_bytes()).hexdigest() == CORE_SHA256
    core = Image(str(core_path))
    platform = Image(str(platform_path))
    table = 0x4cee4
    assert core.u32(table + 0x40) == 0
    rows = [core.read(table + 0x80 + level * 0x3e, 0x3e) for level in range(7)]
    assert [row[0] for row in rows] == SOFTEN
    assert all(row[3] == 30 for row in rows)
    # r6+0x50 is the front flag; the true branch selects the first table.
    beauty = instructions(core, 0x1b730, 3628)
    assert beauty[0x1b7a0] == ("ldrb.w", "r0, [r6, #0x50]")
    assert beauty[0x1b7ac] == ("moveq", "r4, r1")
    literal = 0x1b798 + 4 + 0xc34
    assert core.u32(literal) + 0x1b7a4 + 4 == table
    sces = instructions(core, 0x293f0, 900)
    adjust = instructions(core, 0x300e8, 904)
    assert sces[0x2949c] == ("ldr", "r0, [r4, #4]")
    assert adjust[0x3021e] == ("ldr", "r4, [r6, #4]")
    assert adjust[0x3015e] == ("ldrb.w", "r0, [r8, #0x174]")
    assert adjust[0x302c0] == ("cmp", "r0, #1")
    sces_init = instructions(core, 0x29050, 660)
    assert sces_init[0x2913a] == ("ldrb", "r0, [r4, #8]")
    assert sces_init[0x2913c] == ("lsls", "r0, r0, #0x1f")
    adjust_hwi = instructions(platform, 0x5f74, 492)
    assert adjust_hwi[0x5ff4] == ("ldrb.w", "r3, [r6, #0x1f8]")
    assert adjust_hwi[0x6034] == ("ldrd", "r4, r0, [r6, #0x10]")
    assert adjust_hwi[0x6038] == ("ldr", "r2, [r6, #0x18]")
    print(f"PASS {directory}: front soften={SOFTEN}; fixed feature 0xca=30; "
          "SCES GPU-enable, ImageAdjust enable/stream/pitch, NV21 pointer+4")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vendor-lib-dir", action="append", required=True, type=Path)
    args = parser.parse_args()
    for directory in args.vendor_lib_dir:
        verify(directory)


if __name__ == "__main__":
    main()
