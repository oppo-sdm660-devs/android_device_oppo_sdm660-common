#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Dump the OPPO algorithm registration tables from the two closed-source libs.

Both libraries keep a static, 17-entry algorithm registry in .data.rel.ro.
The table is only referenced from code (there is no symbol for it), so the
base addresses below were recovered from the dispatch loops:

  liboppo_platform_hwi.so  base 0xb8a8  stride 0x38
      +0x00 u64 maskA        (tested against the caller mask in every entry point)
      +0x08 u64 maskB
      +0x10 u64 maskC
      +0x18 u64 maskD
      +0x20 fn register(param, unused, mask)
      +0x24 fn unregister(param, mask)
      +0x28 fn process(frame_ctx, param, mask)
      +0x2c fn updateparameters(param, mask)      (optional)
      +0x30 fn notifycb(int, int)                 (optional)
      +0x34 fn datacb(const void*, int)           (optional)

  libcamera_core_hwi.so    base 0x50cb0 stride 0x18
      +0x00 u64 mask         +0x08 fn register    +0x0c fn unregister
      +0x10 fn updateparameters                    +0x14 fn process

2-byte/1-byte .data.rel.ro slots are left in the file as addend 0 and get
their value from R_ARM_ABS32 relocations, so the function names are read from
the relocation table rather than from the file content.

Usage: ./dump_registry.py <lib.so> [--table platform|core]
"""

import argparse
import re
import sys

from hal1re import Image

TABLES = {
    'platform': {
        'base': 0xB8A8,
        'stride': 0x38,
        'masks': (0x00, 0x08, 0x10, 0x18),
        'functions': {0x20: 'register', 0x24: 'unregister', 0x28: 'process',
                      0x2C: 'updateparams', 0x30: 'notifycb', 0x34: 'datacb'},
    },
    'core': {
        'base': 0x50CB0,
        'stride': 0x18,
        'masks': (0x00,),
        'functions': {0x08: 'register', 0x0C: 'unregister',
                      0x10: 'updateparams', 0x14: 'process'},
    },
}

# bit -> algorithm, from the two tables' mask column plus the .rodata strings
# each implementation prints (e.g. "facebeauty_register", "superphoto_register").
MASK_BITS = {
    0: 'blurless', 1: 'superphoto', 2: 'stfaceblur', 3: 'filter',
    4: 'faceInfo', 5: 'facebeauty', 6: 'bokeh', 7: 'hdr',
    8: 'arc_facebeauty', 9: 'vdscesprocess(SCES)', 10: 'asd(AI scene)',
    11: 'picbest', 12: 'imageadjust', 13: 'portraitLightPreview',
    14: 'llhdr', 15: 'slam', 62: 'offline',
}


def reloc_map(image):
    out = {}
    for rel in image.relocations():
        if rel['type'] == 2 and rel['sym']:
            out[rel['offset']] = rel['sym']
    return out


def main(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument('lib')
    parser.add_argument('--table', choices=sorted(TABLES), default='platform')
    args = parser.parse_args(argv[1:])

    image = Image(args.lib)
    cfg = TABLES[args.table]
    relocs = reloc_map(image)
    print('# %s: %s table @ 0x%x stride 0x%x' % (
        args.lib.split('/')[-1], args.table, cfg['base'], cfg['stride']))
    for index in range(17):
        base = cfg['base'] + index * cfg['stride']
        raw = image.try_read(base, cfg['stride'])
        if raw is None:
            continue
        masks = []
        for off in cfg['masks']:
            value = 0
            for i in range(2):      # masks are 64-bit
                chunk = image.try_read(base + off + i * 4, 4)
                if chunk is None:
                    break
                value |= int.from_bytes(chunk, 'little') << (32 * i)
            masks.append(value)
        primary = masks[0]
        names = []
        for bit in range(63, -1, -1):
            if primary & (1 << bit):
                names.append(MASK_BITS.get(bit, 'bit%d?' % bit))
        funcs = []
        for off, role in sorted(cfg['functions'].items()):
            funcs.append('%s=%s' % (role, relocs.get(base + off, '-')))
        print('[%2d] 0x%08x mask=%s (%s)' % (
            index, base, ' '.join('0x%x' % m for m in masks if m) or '0x0',
            ','.join(names) or '-'))
        print('      ' + '  '.join(funcs))


if __name__ == '__main__':
    main(sys.argv)
