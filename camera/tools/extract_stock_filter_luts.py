#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Extract the embedded Polarr YUV cubes without executing the ARM library.

Produces the checked-in OCam App LUT assets (or a temporary research bundle).
Ordinary App/ROM builds do not run this extractor.
The initFilter Thumb instructions identify the pointer table, rather than a
file offset assumed to survive linker fixups. Unrecognized ABIs fail closed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

SYMBOL = '_ZN12PolarrRender10initFilterE13POLARR_FILTER'
LUT_BYTES = 16 ** 3 * 4
FRONT_TYPES = [0, 1, 2, 3, 5, 4, 10, 6, 7, 8]
BACK_TYPES = [0, 1, 2, 3, 5, 4, 12, 13, 15, 14]
COMMON_IDS = ['SkpZ8ebbz', 'Syt2KeZZf', 'r1JaFSZZG', 'BycYf7WWz', 'rJ5g5vPWG', 'ByYo3If']
FRONT_IDS = COMMON_IDS + ['r1PrfoolG', 'BJJAwoigM', 'rkMhq4ZWf', 'B1aMqioxf']
BACK_IDS = COMMON_IDS + ['B1nmcmWbG', 'ByW7g4bbf', 'rkx5u4Zbf', 'EyxRbD2UCe']
MENU_NAMES = ['和风抹茶', '秋天童话', '青柠奶泡', '海盐泡芙', '蓝莓', '黑咖啡']

LOOKUP_GLSL = '''// Polarr embedded cube: RGB texture channels contain Y,U,V.
vec3 ocamLookupYuv(sampler2D lut, vec3 yuv) {
    vec3 c = clamp(yuv, 0.0, 1.0);
    float x = (c.x * 15.0 + 0.5) / 16.0;
    float y = (c.z * 15.0 + 0.5) / 16.0;
    float z = c.y * 15.0;
    vec3 lo = texture2D(lut, vec2((floor(z) + x) / 16.0, y)).xyz;
    vec3 hi = texture2D(lut, vec2((ceil(z) + x) / 16.0, y)).xyz;
    return mix(lo, hi, fract(z));
}
'''


class Elf32:
    def __init__(self, data):
        self.data = data
        if data[:7] != b'\x7fELF\x01\x01\x01' or self.u16(18) != 40:
            raise ValueError('expected little-endian ARM ELF32')
        phoff, shoff = self.u32(28), self.u32(32)
        phsize, phnum, shsize, shnum = struct.unpack_from('<HHHH', data, 42)
        if phsize != 32 or shsize != 40:
            raise ValueError('unexpected ELF table entries')
        self.loads = [struct.unpack_from('<IIIIIIII', data, phoff + n * phsize)
                      for n in range(phnum)]
        self.loads = [p for p in self.loads if p[0] == 1]
        self.sections = [struct.unpack_from('<IIIIIIIIII', data, shoff + n * shsize)
                         for n in range(shnum)]

    def u16(self, offset):
        return struct.unpack_from('<H', self.data, offset)[0]

    def u32(self, offset):
        return struct.unpack_from('<I', self.data, offset)[0]

    def read(self, address, size):
        for _, offset, va, _, filesz, _, _, _ in self.loads:
            if va <= address and address + size <= va + filesz:
                result = self.data[offset + address - va:offset + address - va + size]
                if len(result) == size:
                    return result
        raise ValueError(f'ELF address 0x{address:x}+{size} is not file backed')

    def symbol(self, name):
        for sec in self.sections:
            if sec[1] != 11:
                continue
            strings = self.sections[sec[6]]
            names = self.data[strings[4]:strings[4] + strings[5]]
            if sec[9] != 16:
                raise ValueError('unexpected ELF symbol width')
            for pos in range(sec[4], sec[4] + sec[5], 16):
                index, value, size, _, _, section = struct.unpack_from('<IIIBBH', self.data, pos)
                end = names.find(b'\0', index)
                if section and names[index:end].decode('ascii') == name:
                    return value & ~1, size
        raise ValueError(f'missing {name}')


def extract(data):
    elf = Elf32(data)
    start, size = elf.symbol(SYMBOL)
    code = elf.read(start, size)
    # ldr r0,[pc,#imm]; add r0,pc; ldr.w r5,[r0,r1,lsl#2].
    load, add = struct.unpack_from('<HH', code, 12)
    if code[:12] != bytes.fromhex('b0b502af86b00446142904d8') or load & 0xff00 != 0x4800 or add != 0x4478 or code[16:20] != bytes.fromhex('50f82150'):
        raise ValueError('unrecognized initFilter pointer-table ABI')
    literal_address = ((start + 12 + 4) & ~3) + (load & 255) * 4
    relative = struct.unpack('<I', elf.read(literal_address, 4))[0]
    table_address = (start + 14 + 4 + relative) & 0xffffffff
    pointers = struct.unpack('<21I', elf.read(table_address, 21 * 4))
    cubes = [elf.read(p, LUT_BYTES) for p in pointers]
    if any(set(c[3::4]) != {255} for c in cubes):
        raise ValueError('unexpected embedded cube alpha/layout')
    return cubes, dict(symbol=SYMBOL, symbolAddress=start,
                       pointerTableAddress=table_address, lutAddresses=list(pointers))


def menu(types, ids, names):
    return [dict(type=t, previewId=i, nameZh=n) for t, i, n in zip(types, ids, names)]


def make_catalog(data, cubes, evidence):
    return dict(schemaVersion=1, dimension=16, textureWidth=256, textureHeight=16,
                channels=4, bytesPerLut=LUT_BYTES, colorSpace='YUV_FULL_RANGE',
                layout='y+16*u+256*v', outputChannels='YUV',
                sampler='GL_LINEAR/GL_CLAMP_TO_EDGE; trilinear Y/U/V',
                sourceSha256=hashlib.sha256(data).hexdigest(), extraction=evidence,
                filterTypes=[dict(id=n, file=f'type-{n}.rgba',
                                  sha256=hashlib.sha256(cube).hexdigest())
                             for n, cube in enumerate(cubes)],
                frontMenu=menu(FRONT_TYPES, FRONT_IDS,
                               MENU_NAMES + ['奶油慕斯', '西柚冰沙', '蓝莓松饼', '雪域芝士']),
                backMenu=menu(BACK_TYPES, BACK_IDS,
                              MENU_NAMES + ['情书', '2046', '重庆森林', '莓果布丁']),
                menuEvidence='R11/R11s OppoCamera: ImageFilterTools a/b/c/d; '
                             'capmode/a.a(I); CameraUtil.d(I) is facing==1',
                rendering='Stock Polarr YUV cube; not libfilter RGB CPU effects. '
                          'RGB Bitmap/preview conversion is a separate OCam stage. '
                          'No claim of stock camera image acceptance.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--lib', required=True, type=Path)
    parser.add_argument('--out-dir', required=True, type=Path)
    args = parser.parse_args()
    data = args.lib.read_bytes()
    cubes, evidence = extract(data)
    args.out_dir.mkdir(parents=True, exist_ok=True)
    for index, cube in enumerate(cubes):
        (args.out_dir / f'type-{index}.rgba').write_bytes(cube)
    (args.out_dir / 'catalog.json').write_text(
        json.dumps(make_catalog(data, cubes, evidence), ensure_ascii=False, indent=2) + '\n')
    (args.out_dir / 'lookup.glsl').write_text(LOOKUP_GLSL)
    print(f'Extracted 21 YUV LUTs from {args.lib} into {args.out_dir}')


if __name__ == '__main__':
    main()
