#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Verify the standalone ARC_DCVR ARM32 boundary in read-only R11/R11s dumps."""
import argparse
import hashlib
from pathlib import Path
import struct

from hal1re import Disassembler, Image

SHA256 = '26275795b0c94837e5f2d8969af49a2e07d3c863b778e641b2ed7111f4f55980'


def verify(directory):
    path = directory / 'libarcsoft_dualcam_refocus_preview.so'
    assert hashlib.sha256(path.read_bytes()).hexdigest() == SHA256
    image = Image(str(path))
    symbols = {s['name']: (s['value'], s['size']) for s in image.syms}
    expected = {'ARC_DCVR_Init': (0x6104, 140),
                'ARC_DCVR_Uninit': (0x60d4, 46),
                'ARC_DCVR_SetCaliData': (0x6190, 88),
                'ARC_DCVR_SetCameraImageInfo': (0x620e, 40),
                'ARC_DCVR_SetImageDegree': (0x61e8, 38),
                'ARC_DCVR_GetDefaultParam': (0x6238, 80),
                'ARC_DCVR_Process': (0x6288, 740)}
    for name, value in expected.items():
        assert symbols[name] == value, name
    decoder = Disassembler(image)
    code = {i['addr']: (i['mnemonic'], i['op_str'])
            for i in decoder.disasm(0x6190, 0x3dc)}
    assert code[0x61be] == ('ldm.w', 'r7, {r0, r1}')  # pointer/length calibration.
    for address, offset in ((0x6218, '0x168'), (0x621e, '0x16c'),
                            (0x6224, '0x170'), (0x622a, '0x174')):
        # All four geometry integers live in the persistent instance.
        assert code[address] == ('str.w', f'r3, [r0, #{offset}]')
    assert code[0x62ac] == ('ldr', 'r6, [sp, #0x98]')
    assert code[0x62ae] == ('ldr.w', 'sb, [sp, #0x9c]')  # fifth/sixth call arguments.
    assert code[0x6362] == ('ldr', 'r3, [r6, #0x18]')  # face count.
    assert code[0x6370] == ('ldr', 'r2, [r6, #0x14]')  # orientation pointer.
    assert code[0x636c] == ('ldr', 'r3, [r6, #0x10]')  # rectangle pointer.
    assert code[0x63d8] == ('ldr', 'r1, [r6, #0x1c]')  # final 32-bit field is read.
    assert code[0x63fc] == ('cmp', 'r3, #4')
    assert code[0x63fe] == ('ble', '#0x6426')  # initial five calls copy input.
    assert code[0x64ee] == ('blx', '#0x4514')  # passthrough memcpy.
    assert struct.unpack('<4i', image.read(0xa438, 16)) == (-1, -1, 100, 1)
    defaults = {i['addr']: (i['mnemonic'], i['op_str'])
                for i in decoder.disasm_arm(0xa414, 36)}
    assert defaults[0xa428] == ('vst1.32', '{d16, d17}, [r3]')  # only 16 default bytes.
    print(f'PASS {directory}: ARC_DCVR six arguments; Param32; defaults=-1,-1,100,1; '
          '512-byte calibration converter; five initial passthrough frames')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--vendor-lib-dir', action='append', required=True, type=Path)
    for directory in parser.parse_args().vendor_lib_dir:
        verify(directory)


if __name__ == '__main__':
    main()
