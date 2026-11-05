#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Extract param_platform_t / frame-ctx field accesses from the OPPO platform lib.

Every algorithm hook in liboppo_platform_hwi.so is a small, branch-light
function that unpacks fields of its inputs before handing the work to a helper.
Tracking which offsets each hook touches is enough to rebuild the two public
layouts (both are opaque C structs with no symbols in the stripped library):

    *_process_hwi(frame_ctx, param, mask)        r0 = frame ctx, r1 = param
    *_register_hwi(param, mask)                  r0 = param
    *_unregister_hwi(param, mask)                r0 = param
    *_updateparameters_hwi(param, mask)          r0 = param
    *_datacb_hwi(const void* data, int size)     r0 = private blob
    fillPlatformParam(dst, param)                r1 = param
    bokeh_setup_parameters_hwi(bokeh_hwi, param) r1 = param

The hooks are compiler-generated straight-line unpackers, so a light scan of
the disassembly text (plus tracking of `add rX, rX, #imm` rebasing) recovers
the accesses precisely.  Output is one line per (symbol, region, offset).

Usage:
  ./extract_offsets.py <liboppo_platform_hwi.so> [--filter REGEX]
"""

import argparse
import re
import sys

from hal1re import Image, Disassembler

FRAME_CTX_SIZE = 0x308
PARAM_SIZE = 0x12EC

ROLES = {
    'process_hwi': {'r0': 'frame_ctx', 'r1': 'param'},
    'register_hwi': {'r0': 'param'},
    'unregister_hwi': {'r0': 'param'},
    'updateparameters_hwi': {'r0': 'param'},
    'datacb_hwi': {'r0': 'data'},
    'notifycb_hwi': {},
    'callback_hwi': {},
    'fillPlatformParam': {'r0': 'platform_dst', 'r1': 'param'},
    'bokeh_setup_parameters_hwi': {'r0': 'bokeh_hwi', 'r1': 'param'},
}

MEM_RE = re.compile(r'\[(\w+)(?:, #(0x[0-9a-f]+|-?0x[0-9a-f]+|-?\d+))?\]')
ADD_RE = re.compile(r'^(\w+), (\w+), #(0x[0-9a-f]+|\d+)$')


def role_for(name):
    for key, roles in ROLES.items():
        if key in name:
            return roles
    return {}


def analyze(image, dis, name):
    syms = [s for s in image.syms if s['name'] == name and s['value'] and s['size']]
    if not syms:
        return []
    sym = syms[0]
    try:
        insns = dis.disasm(sym['value'], sym['size'])
    except SystemExit:
        return []

    roles = role_for(name)
    # disp[reg] = known byte offset from the declared input pointer, or None.
    disp = {}
    for r, role in roles.items():
        disp[r] = (role, 0)
    hits = []

    for rec in insns:
        mnem = rec['mnemonic']
        op = rec['op_str']
        # 1) record accesses through tracked pointers
        for base, off in MEM_RE.findall(op):
            if base not in disp or disp[base] is None:
                continue
            role, known = disp[base]
            val = int(off, 0) if off else 0
            if role == 'frame_ctx' and known + val >= FRAME_CTX_SIZE:
                role = 'param_from_r0'
            size = 1 if mnem.startswith(('ldrb', 'strb')) else \
                2 if mnem.startswith(('ldrh', 'strh')) else \
                8 if mnem.startswith(('ldrd', 'strd')) else 4
            access = 'write' if mnem.startswith('str') else 'read'
            hits.append((name, role, known + val, mnem, access, size))
        # 2) pointer copies and rebasing
        m = re.match(r'^(\w+), (\w+)$', op)
        if mnem in ('mov', 'movs', 'mov.w') and m and m.group(2) in disp:
            disp[m.group(1)] = disp[m.group(2)]
        m = ADD_RE.match(op)
        if mnem in ('add', 'adds', 'add.w') and m:
            dst, src, imm = m.group(1), m.group(2), int(m.group(3), 0)
            if src in disp and disp[src] is not None:
                role, known = disp[src]
                disp[dst] = (role, known + imm)
            else:
                disp[dst] = None
        elif mnem in ('sub', 'subs', 'sub.w') and m:
            dst, src, imm = m.group(1), m.group(2), int(m.group(3), 0)
            if src in disp and disp[src] is not None:
                role, known = disp[src]
                disp[dst] = (role, known - imm)
            else:
                disp[dst] = None
        # 3) anything else writing a register drops the alias
        elif not mnem.startswith(('str', 'vstr', 'cmp', 'tst', 'teq', 'b',
                                  'bl', 'it', 'push', 'pop', 'vpush', 'vpop')):
            m = re.match(r'^(\w+),', op)
            if m and m.group(1) in disp:
                disp[m.group(1)] = None
        # 4) call clobbers the caller-saved registers
        if mnem.startswith('bl') or mnem in ('b', 'b.w'):
            for r in ('r0', 'r1', 'r2', 'r3', 'r12', 'ip'):
                if r in disp:
                    disp[r] = None
    return hits


def main(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument('lib')
    parser.add_argument('--filter', default='hwi')
    args = parser.parse_args(argv[1:])
    image = Image(args.lib)
    dis = Disassembler(image)
    names = sorted({s['name'] for s in image.syms
                    if s['value'] and re.search(args.filter, s['name'])})
    for name in names:
        hits = analyze(image, dis, name)
        if not hits:
            continue
        print('== %s' % name)
        seen = set()
        for _, role, off, mnem, access, size in hits:
            key = (role, off, access)
            if key in seen:
                continue
            seen.add(key)
            print('   %-14s +0x%03x  %-8s %-5s %d bytes' % (
                role, off, mnem, access, size))


if __name__ == '__main__':
    main(sys.argv)
