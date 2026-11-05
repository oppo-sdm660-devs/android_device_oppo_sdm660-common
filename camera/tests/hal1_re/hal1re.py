#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Reproducible read-only analysis helper for the OPPO HAL1 closed-source libs.

The vendor libs are 32-bit ARM ELF, stripped, but keep a full dynamic symbol
table (built with default visibility) and are compiled as Thumb-2.  The ELF
mapping symbols ($a/$t) are stripped, so generic disassemblers default to A32
and produce garbage; every disassembly here forces the Thumb decoder.

Binaries are never copied into the repository: pass their path per invocation.

Usage examples:
  ./hal1re.py info   <lib.so>
  ./hal1re.py syms   <lib.so> [--filter REGEX] [--undef]
  ./hal1re.py strs   <lib.so> [--grep REGEX]
  ./hal1re.py dis    <lib.so> --sym NAME          # or --addr 0x1234 [--len N]
  ./hal1re.py callers<lib.so> --sym NAME          # BL/BLX cross references
  ./hal1re.py xref   <lib.so> --sym NAME          # literal-pool data references
  ./hal1re.py reloc  <lib.so> [--around 0xADDR]
  ./hal1re.py array  <lib.so> --addr 0xADDR --stride N --count M
  ./hal1re.py dump   <lib.so> --addr 0xADDR --len N
"""

import argparse
import hashlib
import re
import struct
import subprocess
import sys

from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection

try:
    import capstone
    from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB, CS_MODE_LITTLE_ENDIAN
    HAVE_CAPSTONE = True
except ImportError:  # pragma: no cover - tool is optional
    HAVE_CAPSTONE = False

PTR_SIZE = 4


# ---------------------------------------------------------------- ELF loading

class Image:
    def __init__(self, path):
        self.path = path
        self.fh = open(path, 'rb')
        self.elf = ELFFile(self.fh)
        self._sections = [s for s in self.elf.iter_sections()]
        self.syms = self._load_symbols()
        self.sym_by_addr = {}
        for s in self.syms:
            if s['value'] and s['type'] in ('FUNC', 'OBJECT', 'IFUNC'):
                self.sym_by_addr.setdefault(s['value'], s)
        self._sorted_syms = None

    # -- sections -----------------------------------------------------------
    def section(self, name):
        return self.elf.get_section_by_name(name)

    def read(self, va, size):
        for s in self._sections:
            addr = s['sh_addr']
            length = s['sh_size']
            if length and s['sh_type'] != 'SHT_NOBITS' and addr <= va < addr + length:
                off = va - addr
                if off + size > length:
                    raise ValueError('read crosses section end at 0x%x' % va)
                return s.data()[off:off + size]
        raise ValueError('address 0x%x is not file-backed' % va)

    def try_read(self, va, size):
        try:
            return self.read(va, size)
        except ValueError:
            return None

    def u32(self, va):
        return struct.unpack('<I', self.read(va, 4))[0]

    def u64(self, va):
        return struct.unpack('<Q', self.read(va, 8))[0]

    # -- symbols ------------------------------------------------------------
    def _load_symbols(self):
        out = []
        for secname in ('.dynsym', '.symtab'):
            sec = self.section(secname)
            if sec is None or sec['sh_type'] == 'SHT_NOBITS':
                continue
            for sym in sec.iter_symbols():
                if not sym.name:
                    continue
                value = sym['st_value']
                stype = sym['st_info']['type'].replace('STT_', '')
                stbind = sym['st_info']['bind'].replace('STB_', '')
                # ARM Thumb functions set bit0 of st_value; strip it for analysis.
                thumb = bool(value & 1) and stype in ('FUNC', 'IFUNC')
                out.append({
                    'name': sym.name,
                    'value': value & ~1 if thumb else value,
                    'thumb': thumb,
                    'size': sym['st_size'],
                    'type': stype,
                    'bind': stbind,
                    'shndx': sym['st_shndx'],
                    'source': secname,
                    # dynsym alone would miss local (hidden) helpers
                })
        return out

    def symbolize(self, addr):
        if self._sorted_syms is None:
            self._sorted_syms = sorted(
                (s for s in self.syms if s['value'] and s['shndx'] != 'SHN_UNDEF'),
                key=lambda s: s['value'])
        best = None
        for s in self._sorted_syms:
            if s['value'] > addr:
                break
            best = s
        if best is None:
            return '0x%x' % addr
        delta = addr - best['value']
        if best['size'] and delta >= best['size']:
            return '0x%x' % addr
        if delta == 0:
            return best['name']
        return '%s+0x%x' % (best['name'], delta)

    def find_sym(self, name):
        hits = [s for s in self.syms if s['name'] == name]
        if not hits:
            hits = [s for s in self.syms if name in s['name']]
        if not hits:
            raise SystemExit('symbol not found: %s' % name)
        if len(hits) > 1:
            exact = [s for s in hits if s['name'] == name]
            hits = exact or hits
        return hits

    # -- PLT / interworking veneers -----------------------------------------
    def plt_symbols(self):
        """Imported symbol per PLT index, in .rel.plt order (linker emits both
        in the same order)."""
        names = []
        for sec in self.elf.iter_sections():
            if sec.name != '.rel.plt' or sec['sh_type'] != 'SHT_REL':
                continue
            symtab = self.elf.get_section(sec['sh_link'])
            for rel in sec.iter_relocations():
                sym = None
                idx = rel['r_info'] >> 8
                if idx and symtab is not None:
                    sym = symtab.get_symbol(idx)
                names.append(sym.name if sym is not None and sym.name else '?')
        return names

    def veneers(self):
        """Map Thumb->ARM interworking veneer address to imported symbol name.

        A veneer is, at a 4-byte aligned address A:
            A+0: bx pc                  (thumb, 0x4778)
            A+2: nop                    (thumb, 0x46c0)
            A+4: ldr ip, [pc]           (arm, literal at A+12)
            A+8: add pc, ip, pc         (arm)
        Branch target = A + 16 + *(A+12), which lands in .plt.
        """
        text = self.section('.text')
        if text is None:
            return {}
        data = text.data()
        base = text['sh_addr']
        plt = self.section('.plt')
        if plt is None:
            return {}
        plt_addr = plt['sh_addr']
        plt_syms = self.plt_symbols()
        out = {}
        for i in range(0, len(data) - 16, 4):
            if data[i:i + 4] != b'\x78\x47\xc0\x46':
                continue
            addr = base + i
            lit = struct.unpack('<I', data[i + 12:i + 16])[0]
            target = (addr + 16 + lit) & 0xffffffff
            k = (target - plt_addr - 0x14) // 12
            if 0 <= k < len(plt_syms) and plt_addr + 0x14 + 12 * k == target:
                out[addr] = plt_syms[k]
        return out

    # -- relocations --------------------------------------------------------
    def relocations(self):
        out = []
        for sec in self.elf.iter_sections():
            if not isinstance(sec, RelocationSection):
                continue
            for rel in sec.iter_relocations():
                off = rel['r_offset']
                info = rel['r_info']
                if sec['sh_type'] == 'SHT_REL':
                    sym_idx = info >> 8
                    rtype = info & 0xff
                    addend = None
                    # ARM: 2 = R_ARM_ABS32 (symbol value), 23 = R_ARM_RELATIVE
                    # (value is [off] + load bias).
                    if rtype in (2, 23):
                        try:
                            addend = self.u32(off)
                        except ValueError:
                            addend = None
                else:
                    sym_idx = info >> 8
                    rtype = info & 0xff
                    addend = rel['r_addend']
                sym = None
                if sym_idx:
                    try:
                        sym = sec.get_section_by_name  # unused, keep simple
                    except Exception:
                        sym = None
                    symtab = self.elf.get_section(sec['sh_link'])
                    if symtab is not None:
                        s = symtab.get_symbol(sym_idx)
                        if s and s.name:
                            sym = s.name
                out.append({'offset': off, 'type': rtype, 'addend': addend,
                            'sym': sym, 'section': sec.name})
        return out


# --------------------------------------------------------------- disassembly

class Disassembler:
    def __init__(self, image):
        if not HAVE_CAPSTONE:
            raise SystemExit('capstone not installed; pip install capstone')
        self.image = image
        self.md = Cs(CS_ARCH_ARM, CS_MODE_THUMB | CS_MODE_LITTLE_ENDIAN)
        self.md.detail = True
        self.md_arm = Cs(CS_ARCH_ARM, CS_MODE_ARM | CS_MODE_LITTLE_ENDIAN)
        self.md_arm.detail = True
        self.veneers = image.veneers()

    def _lit_addr(self, insn, op):
        disp = op.mem.disp
        base = (insn.address + 4) & ~3
        return base + disp

    def disasm_arm(self, addr, size):
        code = self.image.try_read(addr, size)
        if code is None:
            raise SystemExit('no file-backed code at 0x%x' % addr)
        insns = []
        for insn in self.md_arm.disasm(code, addr):
            rec = {'addr': insn.address, 'mnemonic': insn.mnemonic,
                   'op_str': insn.op_str, 'bytes': insn.bytes.hex(),
                   'call': None, 'lit': None, 'lit_value': None}
            m = insn.mnemonic
            if m in ('bl', 'blx') and insn.operands and \
                    insn.operands[0].type == capstone.arm.ARM_OP_IMM:
                rec['call'] = insn.operands[0].imm
            if m.startswith('ldr') and len(insn.operands) >= 2:
                mem = insn.operands[1].mem
                if mem.base == capstone.arm.ARM_REG_PC and mem.index == 0:
                    target = insn.address + 8 + mem.disp
                    rec['lit'] = target
                    raw = self.image.try_read(target, 4)
                    if raw is not None:
                        rec['lit_value'] = struct.unpack('<I', raw)[0]
            insns.append(rec)
        return insns

    def disasm(self, addr, size):
        """Yield dicts with resolved call targets and literal-pool values."""
        code = self.image.try_read(addr, size)
        if code is None:
            raise SystemExit('no file-backed code at 0x%x' % addr)
        insns = []
        for insn in self.md.disasm(code, addr):
            rec = {'addr': insn.address, 'mnemonic': insn.mnemonic,
                   'op_str': insn.op_str, 'bytes': insn.bytes.hex(),
                   'call': None, 'lit': None, 'lit_value': None}
            m = insn.mnemonic
            if m.startswith('bl') and insn.operands and \
                    insn.operands[0].type == capstone.arm.ARM_OP_IMM:
                rec['call'] = insn.operands[0].imm
            if m.startswith('ldr') and len(insn.operands) >= 2:
                mem = insn.operands[1].mem
                if mem.base == capstone.arm.ARM_REG_PC:
                    target = self._lit_addr(insn, insn.operands[1])
                    rec['lit'] = target
                    if mem.index == 0:
                        raw = self.image.try_read(target, 4)
                        if raw is not None:
                            rec['lit_value'] = struct.unpack('<I', raw)[0]
            if m in ('add', 'adr') and len(insn.operands) >= 2 and \
                    insn.operands[1].type == capstone.arm.ARM_OP_MEM and \
                    insn.operands[1].mem.base == capstone.arm.ARM_REG_PC:
                rec['lit'] = self._lit_addr(insn, insn.operands[1])
                rec['pc_add'] = (insn.address + 4)
            insns.append(rec)
        return insns

    def annotate(self, insns):
        for rec in insns:
            notes = []
            if rec['call'] is not None:
                target = rec['call']
                imported = self.veneers.get(target)
                if imported is not None:
                    notes.append('-> %s [imported]' % imported)
                else:
                    notes.append('-> %s' % self.image.symbolize(target))
            if rec['lit'] is not None:
                va = rec['lit']
                value = rec['lit_value']
                note = 'lit@0x%x' % va
                if value is not None:
                    note += ' = 0x%x' % value
                    sym = self.image.symbolize(value)
                    if not sym.startswith('0x'):
                        note += ' (%s)' % sym
                    elif value == 0:
                        note += ' (0)'
                if rec.get('pc_add'):
                    note += ' ; pc-rel addr -> 0x%x' % (va + value if value else 0)
                notes.append(note)
            rec['note'] = '  '.join(notes)
        return insns


# ------------------------------------------------------------------- outputs

def cmd_info(img, args):
    data = open(img.path, 'rb').read()
    elf = img.elf
    print('file      : %s' % img.path)
    print('size      : %d bytes' % len(data))
    print('sha256    : %s' % hashlib.sha256(data).hexdigest())
    print('md5       : %s' % hashlib.md5(data).hexdigest())
    print('class     : ELF%d %s %s' % (
        32 if elf.elfclass == 32 else 64,
        elf.header['e_machine'], elf.header['e_ident']['EI_DATA']))
    build_id = None
    sec = img.section('.note.gnu.build-id')
    if sec is not None:
        raw = sec.data()
        build_id = raw[12:].hex() if len(raw) > 12 else raw.hex()
    print('build-id  : %s' % build_id)
    for name in ('.dynsym', '.symtab', '.gnu_debugdata'):
        s = img.section(name)
        print('%-16s: %s' % (name, 'absent' if s is None or s['sh_type'] == 'SHT_NOBITS'
                             else '%d bytes' % s['sh_size']))
    for s in img._sections:
        if s.name in ('.text', '.rodata', '.data', '.bss', '.data.rel.ro',
                      '.plt', '.ARM.exidx'):
            print('%-16s: va 0x%08x size 0x%x' % (s.name, s['sh_addr'], s['sh_size']))
    if args.imports:
        print('\nimported symbols (UND):')
        for s in sorted(img.syms, key=lambda s: s['name']):
            if s['shndx'] == 'SHN_UNDEF' and s['type'] != 'NOTYPE':
                print('  %s' % s['name'])


def cmd_syms(img, args):
    rows = []
    for s in img.syms:
        if s['shndx'] == 'SHN_UNDEF' and not args.undef:
            continue
        if args.filter and not re.search(args.filter, s['name']):
            continue
        rows.append(s)
    rows.sort(key=lambda s: (s['value'], s['name']))
    for s in rows:
        print('0x%08x %6d %-6s %-6s %s' % (
            s['value'], s['size'], s['type'], s['bind'], s['name']))


def cmd_strs(img, args):
    secs = ['.rodata']
    if args.allsections:
        secs = None
    for s in img._sections:
        if secs is not None and s.name not in secs:
            continue
        if s['sh_type'] == 'SHT_NOBITS':
            continue
        data = s.data()
        addr = s['sh_addr']
        start = None
        for i, byte in enumerate(data):
            printable = 32 <= byte < 127
            if printable and start is None:
                start = i
            elif not printable and start is not None:
                if i - start >= args.minlen:
                    text = data[start:i].decode('ascii')
                    if not args.grep or re.search(args.grep, text):
                        print('%s 0x%08x %s' % (s.name, addr + start, text))
                start = None


def _resolve(img, args):
    if args.sym:
        hits = img.find_sym(args.sym)
        return hits[0]['value'], hits[0]['size'], hits[0]['name']
    if args.addr is not None:
        start = int(args.addr, 0)
        size = args.len if args.len is not None else 0x100
        return start, size, img.symbolize(start)
    raise SystemExit('need --sym or --addr')


def cmd_dis(img, args):
    dis = Disassembler(img)
    start, size, name = _resolve(img, args)
    if args.len is None and size:
        size = size
    print('%s @ 0x%08x (%d bytes) [thumb]' % (name, start, size))
    if getattr(args, 'mode', 'thumb') == 'arm':
        insns = dis.annotate(dis.disasm_arm(start, size))
    else:
        insns = dis.annotate(dis.disasm(start, size))
    for rec in insns:
        print('  0x%08x: %-28s %-12s %s' % (
            rec['addr'], rec['bytes'], rec['mnemonic'] + ' ' + rec['op_str'],
            rec['note']))
    if args.json:
        import json
        print(json.dumps([{k: v for k, v in i.items() if k != 'bytes'}
                          for i in insns], indent=1))


def _iter_calls(img):
    """Linear sweep of .text decoding BL/BLX and PC-relative literal loads."""
    dis = Disassembler(img)
    text = img.section('.text')
    data = text.data()
    base = text['sh_addr']
    for insn in dis.md.disasm(data, base):
        if insn.mnemonic.startswith('bl') and insn.operands and \
                insn.operands[0].type == capstone.arm.ARM_OP_IMM:
            yield insn.address, insn.operands[0].imm, None
        elif insn.mnemonic.startswith('ldr') and len(insn.operands) >= 2:
            mem = insn.operands[1].mem
            if mem.base == capstone.arm.ARM_REG_PC and mem.index == 0:
                target = ((insn.address + 4) & ~3) + mem.disp
                raw = img.try_read(target, 4)
                if raw is not None:
                    yield insn.address, target, struct.unpack('<I', raw)[0]


def cmd_callers(img, args):
    start, size, name = _resolve(img, args)
    for addr, target, literal in _iter_calls(img):
        if literal is not None:
            continue
        if target == start:
            sym = img.symbolize(addr)
            print('call 0x%08x (in %s) -> %s' % (addr, sym, name))


def cmd_xref(img, args):
    start, size, name = _resolve(img, args)
    print('references to %s (0x%x):' % (name, start))
    for addr, target, literal in _iter_calls(img):
        if literal is not None and literal == start:
            print('  literal pool 0x%08x used by insn 0x%08x (%s)' % (
                target, addr, img.symbolize(addr)))
    for rel in img.relocations():
        if rel['addend'] == start and rel['type'] == 2:
            print('  R_ARM_RELATIVE @0x%08x (%s)' % (
                rel['offset'], img.symbolize(rel['offset'])))


def cmd_reloc(img, args):
    around = int(args.around, 0) if args.around else None
    for rel in img.relocations():
        if around is not None and not (around <= rel['offset'] < around + 0x100):
            continue
        kind = {2: 'ABS32', 21: 'GLOB_DAT', 22: 'JUMP_SLOT', 23: 'RELATIVE'}.get(
            rel['type'], str(rel['type']))
        extra = ''
        if rel['sym']:
            extra += ' target=%s' % rel['sym']
        if rel['addend']:
            extra += ' addend=0x%x' % rel['addend']
        if rel['type'] == 23:
            extra += ' slot=0x%x' % (rel['addend'] or 0)
        print('0x%08x %-10s%s' % (rel['offset'], kind, extra))


def cmd_veneers(img, args):
    ven = img.veneers()
    for addr in sorted(ven):
        print('0x%08x -> %s' % (addr, ven[addr]))


def cmd_array(img, args):
    addr = int(args.addr, 0)
    stride = args.stride
    count = args.count
    for i in range(count):
        base = addr + i * stride
        print('--- [%d] 0x%08x (%s) ---' % (i, base, img.symbolize(base)))
        for off in range(0, stride, 4):
            slot = base + off
            raw = img.try_read(slot, 4)
            if raw is None:
                print('  +0x%02x  <no file data>' % off)
                continue
            value = struct.unpack('<I', raw)[0]
            print('  +0x%02x  0x%08x  %s' % (off, value, img.symbolize(value)))


def cmd_dump(img, args):
    addr = int(args.addr, 0)
    size = args.len
    data = img.try_read(addr, size)
    if data is None:
        print('not file-backed: 0x%x+0x%x' % (addr, size))
        return
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        hexpart = ' '.join('%02x' % b for b in chunk)
        asciipart = ''.join(chr(b) if 32 <= b < 127 else '.' for b in chunk)
        print('0x%08x  %-47s  %s' % (addr + i, hexpart, asciipart))


def main(argv):
    # The library path comes right after the subcommand: `hal1re.py <cmd> <lib.so>`.
    if len(argv) < 2:
        raise SystemExit('usage: hal1re.py <cmd> <lib.so> [options]')
    lib_path, argv = argv[1], [argv[0]] + argv[2:]
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='cmd', required=True)

    p = sub.add_parser('info')
    p.add_argument('--imports', action='store_true')
    p.set_defaults(func=cmd_info)

    p = sub.add_parser('syms')
    p.add_argument('--filter')
    p.add_argument('--undef', action='store_true')
    p.set_defaults(func=cmd_syms)

    p = sub.add_parser('strs')
    p.add_argument('--grep')
    p.add_argument('--minlen', type=int, default=4)
    p.add_argument('--allsections', action='store_true')
    p.set_defaults(func=cmd_strs)

    for name, func in (('dis', cmd_dis), ('callers', cmd_callers),
                       ('xref', cmd_xref)):
        p = sub.add_parser(name)
        p.add_argument('--sym')
        p.add_argument('--addr')
        p.add_argument('--len', type=lambda v: int(v, 0))
        if name == 'dis':
            p.add_argument('--json', action='store_true')
            p.add_argument('--mode', default='thumb', choices=('thumb', 'arm'))
        p.set_defaults(func=func)

    p = sub.add_parser('veneers')
    p.set_defaults(func=cmd_veneers)

    p = sub.add_parser('reloc')
    p.add_argument('--around')
    p.set_defaults(func=cmd_reloc)

    p = sub.add_parser('array')
    p.add_argument('--addr', required=True)
    p.add_argument('--stride', type=lambda v: int(v, 0), required=True)
    p.add_argument('--count', type=int, required=True)
    p.set_defaults(func=cmd_array)

    p = sub.add_parser('dump')
    p.add_argument('--addr', required=True)
    p.add_argument('--len', type=lambda v: int(v, 0), default=64)
    p.set_defaults(func=cmd_dump)

    args = parser.parse_args(argv)
    img = Image(lib_path)
    args.func(img, args)


if __name__ == '__main__':
    main(sys.argv[1:])
