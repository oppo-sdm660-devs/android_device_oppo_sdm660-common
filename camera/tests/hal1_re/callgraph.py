#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Call-graph extraction for the stripped OPPO HAL1 libs.

The libraries keep a full .dynsym (default-visibility build) and are Thumb-2,
but their internal `$a`/`$t` mapping symbols are stripped, so binutils objdump
cannot decode them at all and llvm-objdump needs `--triple=thumbv7a-...`.
This tool decodes .text as Thumb-2, resolves BL/BLX targets through the
Thumb->PLT interworking veneers at the end of .text, and reports:

  --callees SYM   transitive callee tree (depth-limited), with call sites
  --callers SYM   direct callers, with call sites
  --imports SYM   imported (PLT) functions reachable from SYM
  --roots         functions that are never called from inside the library

Usage:
  ./callgraph.py <lib.so> --callees _ZN10oppocamera8ocamcore11processNodeEPv
  ./callgraph.py <lib.so> --callers _Z13llhdr_processPvS_
"""

import argparse
import sys

import capstone
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB, CS_MODE_LITTLE_ENDIAN

from hal1re import Image


def scan_edges(image):
    """Return {caller_symbol: [(callsite, callee_name), ...]} for the whole .text."""
    text = image.section('.text')
    if text is None:
        return {}, {}
    md = Cs(CS_ARCH_ARM, CS_MODE_THUMB | CS_MODE_LITTLE_ENDIAN)
    md.detail = True
    veneers = image.veneers()
    data = text.data()
    base = text['sh_addr']
    edges = {}
    for insn in md.disasm(data, base):
        if not insn.mnemonic.startswith('bl'):
            continue
        if not insn.operands or insn.operands[0].type != capstone.arm.ARM_OP_IMM:
            continue
        target = insn.operands[0].imm
        callee = veneers.get(target)
        if callee is None:
            callee = image.symbolize(target)
        caller = image.symbolize(insn.address).split('+')[0]
        edges.setdefault(caller, []).append((insn.address, callee))
    return edges, veneers


def strip_offset(name):
    return name.split('+')[0]


def build_index(image):
    return {s['name'] for s in image.syms if s['value'] and s['type'] == 'FUNC'}


def callees_tree(image, edges, index, root, depth, prefix='', seen=None):
    if seen is None:
        seen = set()
    name = strip_offset(root)
    if depth <= 0 or name in seen:
        return
    seen.add(name)
    for callsite, callee in edges.get(name, []):
        imported = '+' not in callee and callee not in index
        print('%s0x%08x -> %s%s' % (prefix, callsite, callee,
                                    ' [imported]' if imported else ''))
        callees_tree(image, edges, index, callee, depth - 1,
                     prefix + '    ', seen)


def main(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument('lib')
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument('--callees')
    group.add_argument('--callers')
    group.add_argument('--roots', action='store_true')
    parser.add_argument('--depth', type=int, default=3)
    args = parser.parse_args(argv[1:])

    image = Image(args.lib)
    edges, veneers = scan_edges(image)
    index = build_index(image)

    if args.roots:
        called = {strip_offset(callee) for items in edges.values()
                  for _, callee in items}
        roots = sorted(index - called & set(s['name'] for s in image.syms))
        for name in roots:
            if name in called:
                continue
            size = next((s['size'] for s in image.syms if s['name'] == name), 0)
            print('%6d  %s' % (size, name))
        return

    if args.callees:
        callees_tree(image, edges, index, args.callees, args.depth)
        return

    # --callers
    target = args.callers
    found = False
    for caller, items in sorted(edges.items()):
        for callsite, callee in items:
            if strip_offset(callee) == strip_offset(target):
                print('0x%08x in %s' % (callsite, caller))
                found = True
    if not found:
        print('no internal caller found for %s' % target)


if __name__ == '__main__':
    main(sys.argv)
