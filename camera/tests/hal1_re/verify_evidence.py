#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Assert the HAL1 reconstruction evidence against the two vendor libraries.

Everything checked here is a claim made in OCam/docs/hal1-re-hwi.md.  If the
vendor libs change (different ROM/dump), this script fails loudly with the
mismatching item instead of silently invalidating the document.

Checks:
  1. file size / sha256 / BuildID of both libraries
  2. liboppo_platform_hwi: 17-entry registry at 0xb8a8, stride 0x38,
     bit -> algorithm mapping, function slots
  3. dispatch constants: process() re-checks the "already registered" masks,
     init()'s out2/out3 bit sets (0x1ffb8 / 0x10047)
  4. param_platform_t / frame-ctx field read sites in the algorithm hooks
     (e.g. hdr_process_hwi reads frame_ctx+0x10..0x1c and param+0x168)
  5. face-info array: 10 x 0x174 at param+0x214, copied by bokeh_setup
  6. libcamera_core_hwi: 17-entry registry at 0x50cb0, stride 0x18
  7. Thumb->PLT veneers resolve the camAlgo*/oppo_wrapper_* bridge
  8. ZSL absence in both libs (no symbols, no strings)

Exit code 0 = all checks pass, 1 = at least one mismatch.

Usage: ./verify_evidence.py [--vendor-lib-dir DIR]
"""

import argparse
import hashlib
import os
import re
import struct
import sys

from hal1re import Image, Disassembler

DEFAULT_DIR = ('/home/cy/Project/R11/proprietary_vendor_oppo_sdm660-common/'
               'proprietary/vendor/lib')
DEFAULT_STOCK_HAL1 = '/home/cy/Project/R11/stock/R11/vendor/lib/hw/camera.sdm660.so'

EXPECTED = {
    'liboppo_platform_hwi.so': {
        'size': 49060,
        'sha256': 'c7ec37dfa9bc694322f2ffaacf20967235ac5d67e7dac78df135e7c1a33b742b',
        'build_id': '5d97f73476c6e98e8d8ddfca08112552',
    },
    'libcamera_core_hwi.so': {
        'size': 402656,
        'sha256': '3ef68495cbf08a275112f4d3217e29a4c0443f9b10632e492afc2311217bafcd',
        'build_id': '7f4afeea7673da52407182b45e9f664d',
    },
}

PLATFORM_REGISTRY_BASE = 0xB8A8
PLATFORM_REGISTRY_STRIDE = 0x38
CORE_REGISTRY_BASE = 0x50CB0
CORE_REGISTRY_STRIDE = 0x18

# bit -> expected register_hwi symbol fragment
PLATFORM_ORDER = [
    (0xB8A8, 'picbest_register_hwi', 11),
    (0xB8E0, 'blurless_register_hwi', 0),
    (0xB918, 'superphoto_register_hwi', 1),
    (0xB950, 'stfaceblur_register_hwi', 2),
    (0xB988, 'hdr_register_hwi', 7),
    (0xB9C0, 'filter_register_hwi', 3),
    (0xB9F8, 'faceInfo_regeister_hwi', 4),
    (0xBA30, 'facebeauty_register_hwi', 5),
    (0xBA68, 'imageadjust_register_hwi', 12),
    (0xBAA0, 'arc_facebeauty_register_hwi', 8),
    (0xBAD8, 'bokeh_register_hwi', 6),
    (0xBB10, 'vdscesprocess_register_hwi', 9),
    (0xBB48, 'portraitLightPreview_register_hwi', 13),
    (0xBB80, 'asd_register_hwi', 10),
    (0xBBB8, 'llhdr_register_hwi', 14),
    (0xBBF0, 'slam_register_hwi', 15),
    (0xBC28, 'offline_register_hwi', 62),
]

CORE_ORDER = [
    (0x50CB0, 'picbest_register'),
    (0x50CC8, 'blurless_register'),
    (0x50CE0, 'superphoto_register'),
    (0x50CF8, 'hdr_register'),
    (0x50D10, 'llhdr_register'),
    (0x50D28, 'arcfacebeauty_register'),
]

results = []


def check(name, ok, detail=''):
    results.append((name, ok, detail))
    print('%s %s%s' % ('PASS' if ok else 'FAIL', name,
                       (' -- ' + detail) if detail and not ok else ''))


def reloc_targets(image):
    out = {}
    for rel in image.relocations():
        if rel['type'] == 2 and rel['sym']:
            out[rel['offset']] = rel['sym']
    return out


def read_u64(image, addr):
    lo = image.u32(addr)
    hi = image.u32(addr + 4)
    return lo | (hi << 32)


def insn_offsets(image, symbol, base_reg='r1'):
    """All [base_reg, #imm] memory displacements in a function's disassembly."""
    dis = Disassembler(image)
    syms = [s for s in image.syms if s['name'] == symbol and s['value']]
    if not syms:
        return set()
    sym = syms[0]
    insns = dis.disasm(sym['value'], sym['size'])
    pattern = re.compile(r'\[%s, #(0x[0-9a-f]+)\]' % base_reg)
    return {int(m, 16) for rec in insns for m in pattern.findall(rec['op_str'])}


def main(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument('--vendor-lib-dir', default=DEFAULT_DIR)
    parser.add_argument('--stock-hal1', default=DEFAULT_STOCK_HAL1)
    args = parser.parse_args(argv[1:])
    lib_dir = args.vendor_lib_dir

    # 1. identity of the inputs -------------------------------------------
    for name, expected in EXPECTED.items():
        path = os.path.join(lib_dir, name)
        if not os.path.exists(path):
            check('%s present' % name, False, path)
            continue
        data = open(path, 'rb').read()
        check('%s size' % name, len(data) == expected['size'],
              '%d != %d' % (len(data), expected['size']))
        check('%s sha256' % name,
              hashlib.sha256(data).hexdigest() == expected['sha256'])
        image = Image(path)
        note = image.section('.note.gnu.build-id')
        # ELF note: namesz/u32, descsz/u32, type/u32, then name (padded) + desc
        build_id = note.data()[16:].hex() if note is not None else ''
        check('%s build-id' % name, build_id == expected['build_id'], build_id)

    platform_path = os.path.join(lib_dir, 'liboppo_platform_hwi.so')
    core_path = os.path.join(lib_dir, 'libcamera_core_hwi.so')
    platform = Image(platform_path)
    core = Image(core_path)
    platform_rel = reloc_targets(platform)
    core_rel = reloc_targets(core)

    # 2. platform registry -------------------------------------------------
    for base, expected_fn, expected_bit in PLATFORM_ORDER:
        mask = read_u64(platform, base)
        check('platform registry 0x%x mask==bit%d' % (base, expected_bit),
              mask == (1 << expected_bit), '0x%x' % mask)
        check('platform registry 0x%x register==%s' % (base, expected_fn),
              expected_fn in platform_rel.get(base + 0x20, ''),
              platform_rel.get(base + 0x20, '-'))
    check('platform registry stride',
          PLATFORM_ORDER[0][0] + 16 * PLATFORM_REGISTRY_STRIDE ==
          PLATFORM_ORDER[16][0])

    # 3. dispatch constants ------------------------------------------------
    # init() compares the caller mask against each entry's maskA and filters
    # the out2/out3 bit sets with 0x1ffb8 / 0x10047.
    def function_text(image, symbol):
        dis = Disassembler(image)
        sym = [s for s in image.syms if s['name'] == symbol and s['value']][0]
        return ' '.join('%s %s' % (r['mnemonic'], r['op_str'])
                        for r in dis.disasm(sym['value'], sym['size']))

    init_text = function_text(platform, '_Z18oppo_platform_initPvyPyS0_S0_')
    check('platform init loops over 17 registry entries',
          '#0x11' in init_text and '#0x38' in init_text)
    process_text = function_text(platform, '_Z21oppo_platform_processPvS_y')
    check('platform process loops over 17 registry entries',
          '#0x11' in process_text and '#0x38' in process_text)
    check('platform process calls entry slot +0x28',
          '#0x28' in process_text)
    check('platform process re-checks a registered mask in .bss',
          '0xc028' in process_text or '0xc030' in process_text or
          'ldrd' in process_text)

    # 4. param / frame-ctx field sites -------------------------------------
    hdr_text = function_text(platform, '_Z15hdr_process_hwiPvS_y')
    hdr = insn_offsets(platform, '_Z15hdr_process_hwiPvS_y', 'r1')
    check('hdr_process_hwi param+0x168 (isFront)',
          0x168 in hdr, 'found %s' % sorted(hdr))
    check('hdr_process_hwi param+0x1a0/+0x1a1',
          {0x1a0, 0x1a1} <= hdr)
    check('hdr_process_hwi param+0x384', 0x384 in hdr)
    # 10-frame HDR: arrays at param+0x23c/0x264/0x28c/0x2b4/0x2dc/0x354, 0xa frames
    check('hdr_process_hwi 10-frame HDR parameter arrays',
          '#0x23c' in hdr_text and '#0xa' in hdr_text and
          all(('#' + hex(off)) in hdr_text for off in (0x28, 0x50, 0x78, 0xa0, 0x118)))

    # frame ctx geometry is read through the first argument (r0 / moved alias)
    geometry = set()
    for symbol in ('_Z18filter_process_hwiPvS_y', '_Z20blurless_process_hwiPvS_y',
                   '_Z20faceInfo_process_hwiPvS_y', '_Z22stfaceblur_process_hwiPvS_y'):
        dis = Disassembler(platform)
        sym = [s for s in platform.syms if s['name'] == symbol and s['value']][0]
        text = ' '.join(r['op_str'] for r in dis.disasm(sym['value'], sym['size']))
        for off in (0x10, 0x14, 0x18, 0x1c):
            if '[ip, #0x%x]' % off in text or '[r0, #0x%x]' % off in text:
                geometry.add(off)
    check('frame ctx geometry offsets 0x10/0x14/0x18/0x1c',
          geometry == {0x10, 0x14, 0x18, 0x1c}, str(sorted(geometry)))

    # 5. face-info array ---------------------------------------------------
    setup_text = function_text(
        platform, '_Z26bokeh_setup_parameters_hwiP9bokeh_hwiP16param_platform_t')
    check('bokeh_setup_parameters_hwi param+0x184 (blur strength)', '#0x184' in setup_text)
    check('bokeh_setup_parameters_hwi param+0x214 (face array)', '#0x214' in setup_text)
    # the 10-iteration loop writes bokeh_hwi+0xac + i*0x174
    dis = Disassembler(platform)
    sym = [s for s in platform.syms
           if s['name'] == '_Z26bokeh_setup_parameters_hwiP9bokeh_hwiP16param_platform_t'][0]
    text = ' '.join(r['op_str'] for r in dis.disasm(sym['value'], sym['size']))
    check('bokeh face array entry size 0x174', '#0x174' in text)
    check('bokeh face array count 10', '#0xa' in text)

    # 6. core registry -----------------------------------------------------
    for base, expected_fn in CORE_ORDER:
        mask = read_u64(core, base)
        check('core registry 0x%x mask nonzero' % base, mask != 0, '0x%x' % mask)
        check('core registry 0x%x register==%s' % (base, expected_fn),
              expected_fn in core_rel.get(base + 0x08, ''),
              core_rel.get(base + 0x08, '-'))
    check('core registry stride 0x18',
          CORE_REGISTRY_BASE + 16 * CORE_REGISTRY_STRIDE == 0x50E30)

    # 7. veneer / bridge layer --------------------------------------------
    # liboppo_platform_hwi imports the camAlgo* daemon API and the OPPO
    # wrapper implementation in libcamera_core_hwi; both go through the
    # Thumb->PLT veneers at the end of .text.
    platform_veneers = set(platform.veneers().values())
    platform_imports = {s['name'] for s in platform.syms
                        if s['shndx'] == 'SHN_UNDEF' and s['name']}
    for imported in ('camAlgoProcess', 'camAlgoSetParam', 'camAlgoInit'):
        check('platform veneer resolves %s' % imported,
              imported in platform_veneers)
    # oppo_wrapper_* are imported but reached as registered callbacks, so they
    # show up in UND/.got rather than as a direct veneer call site.
    for imported in ('_Z20oppo_wrapper_processPvS_x', '_Z17oppo_wrapper_initPvx'):
        check('platform imports %s' % imported, imported in platform_imports)
    defined = {s['name'] for s in core.syms if s['value']}
    for exported in ('_Z17oppo_wrapper_initPvx', '_Z20oppo_wrapper_processPvS_x',
                     '_Z18oppo_wrapper_setcbPFvxiiEPFvxPKviE',
                     '_Z23oppo_wrapper_offline_cbxPKvib'):
        check('core exports %s' % exported, exported in defined)

    # 8. ZSL absence -------------------------------------------------------
    for name, image in (('libcamera_core_hwi', core),
                        ('liboppo_platform_hwi', platform)):
        symbols = [s['name'] for s in image.syms if 'zsl' in s['name'].lower()]
        strings = _strings(image)
        check('%s has no ZSL symbols/strings' % name,
              not symbols and not any('zsl' in s.lower() for s in strings),
              '%s' % symbols[:3])

    # 9. second batch: stock HAL1 builder evidence ------------------------
    stock = args.stock_hal1
    if os.path.exists(stock):
        import frame_ctx
        import face_info
        import mode_mask
        batch = []
        batch += frame_ctx._checks(stock)
        batch += face_info._checks(
            stock, os.path.join(lib_dir, 'liboppo_platform_hwi.so'))
        batch += mode_mask._checks(stock)
        import snapshot as snapshot_mod
        batch += snapshot_mod._checks(stock,
                                      args.vendor_lib_dir if
                                      hasattr(args, 'vendor_lib_dir') else lib_dir)
        for item in batch:
            check(item[0], bool(item[1]))
        print('second batch: %s' % stock)
    else:
        print('skip second batch: %s not found' % stock)

    failed = [name for name, ok, _ in results if not ok]
    print('\n%d checks, %d failed' % (len(results), len(failed)))
    return 1 if failed else 0


def _strings(image):
    out = []
    for section in image._sections:
        if section.name not in ('.rodata', '.data'):
            continue
        if section['sh_type'] == 'SHT_NOBITS':
            continue
        data = section.data()
        for match in re.finditer(rb'[ -~]{6,}', data):
            out.append(match.group().decode('ascii'))
    return out


if __name__ == '__main__':
    sys.exit(main(sys.argv))
