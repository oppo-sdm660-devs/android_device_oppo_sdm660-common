#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Frame-ctx (0x308) / param_platform_t (0x12EC) builder evidence from OPPO HAL1.

The first HAL1 batch (task-55) only had the two algorithm libraries:
liboppo_platform_hwi.so / libcamera_core_hwi.so.  They are *callees*: neither
builds the public structs.  The builder is the stock HAL1 module
`vendor/lib/hw/camera.sdm660.so` (the proprietary OPPO/QCamera2 HAL), which is
stripped of .symtab but exports 3262 named functions in .dynsym, so the whole
preview/snapshot algorithm path is name-resolvable.

Recovered anchors (all instruction addresses are in camera.sdm660.so, Thumb-2):

  QCamera2HardwareInterface (this):
      param_platform_t   = this + 0x1718   (0x12EC bytes)
      frame ctx (0x308)  = this + 0x2A08
  OppoImageBokehInt (bokeh HAL1 object):
      param_platform_t   = this + 0x0070   (0x12EC bytes)
      frame ctx / image descriptor array = this + 0x1360 (10 x buffer_def_t, 0x40 each)

  algoPreview()  fills both structures per preview frame and calls
      oppo_platform_process(this+0x2A08, this+0x1718, preview_mask)          @0x12b2d4
  algoAfterCPP() calls the same pair for the snapshot path.
  algoReset()    calls oppo_platform_uninit(this+0x1718, mask) x3 and then
      memclr(this+0x1718, 0x12EC) x3                                        @0x12a3fc+

Usage:
  ./frame_ctx.py [--stock-hal1 PATH]            # print the evidence table
  ./frame_ctx.py [--stock-hal1 PATH] --verify   # assertions, exit 1 on mismatch
"""

import argparse
import os
import re
import sys

from hal1re import Image, Disassembler, function_calls

DEFAULT_STOCK = ('/home/cy/Project/R11/stock/R11/vendor/lib/hw/camera.sdm660.so')
_ALGO_PREVIEW = ('_ZN7qcamera25QCamera2HardwareInterface11algoPreview'
                 'EP21mm_camera_super_buf_tP17mm_camera_buf_defPNS_13QCameraStreamEPvy')
_ALGO_RESET = '_ZN7qcamera25QCamera2HardwareInterface9algoResetEv'
_BOKEH_PROCESS = '_ZN7qcamera17OppoImageBokehInt7processEv'
EXPECTED = {
    'size': 1742388,
    'sha256': '226d41ad86eff0199aaff356f77f0c6a7b786da320efad0bb130500ccd166863',
    'build_id': '3a0843572d64e6fe96ef49d49f183090',
}

# this-relative member offsets proving the two public struct bases.
PARAM_BASE = 0x1718
CTX_BASE = 0x2A08
BOKEH_PARAM_BASE = 0x0070
BOKEH_CTX_BASE = 0x1360
BOKEH_BUFDEF_BASE = 0x1360
BOKEH_BUFDEF_STRIDE = 0x40
DESC_BASE_IN_BOKEH = 0x0284          # param+0x214
DESC_STRIDE = 0x174

# algoPreview writes (this-relative offsets).  base -> (public offset, kind)
PREVIEW_PARAM_WRITES = [
    (0x1718, 'param+0x000 u32 width'),
    (0x171c, 'param+0x004 u32 height'),
    (0x1720, 'param+0x008 u32 aligned stride'),
    (0x1724, 'param+0x00c u32 aligned slice'),
    (0x1734, 'param+0x01c u32 constant 1'),
    (0x173c, 'param+0x024 u32 from this+0xe1c'),
    (0x176c, 'param+0x054 u32 flip mode (getFlipMode)'),
    (0x1878, 'param+0x160 u32 visiontech filter type'),
    (0x27f8, 'param+0x10e0 u32 OAR need-stop'),
]
PREVIEW_CTX_WRITES = [
    (0x2a0c, 'ctx+0x004 u32 (source [sp+0x34], unresolved)'),
    (0x2a14, 'ctx+0x00c u32 QCameraStream+0x20c'),
    (0x2a18, 'ctx+0x010 u32 width'),
    (0x2a1c, 'ctx+0x014 u32 height'),
    (0x2a20, 'ctx+0x018 u32 stride'),
    (0x2a24, 'ctx+0x01c u32 slice'),
    (0x2a28, 'ctx+0x020 u32 QCameraStream+0x1c'),
    (0x2a30, 'ctx+0x028 f64 frame timestamp (s + us*1e-6)'),
    (0x2a3c, 'ctx+0x034 u32 constant 1'),
    (0x2c88, 'ctx+0x280 u32 constant 1'),
    (0x2c94, 'ctx+0x28c u32 (source [sp+0x34], unresolved)'),
    (0x2c9c, 'ctx+0x294 u32 QCameraStream+0x20c'),
    (0x2ca0, 'ctx+0x298 u32'),
    (0x2ca4, 'ctx+0x29c u32'),
    (0x2ca8, 'ctx+0x2a0 u32'),
    (0x2cac, 'ctx+0x2a4 u32'),
]

# core library consumers of the frame ctx (first argument of *_process(void*,void*)).
CORE_CTX_CONSUMERS = [
    ('libcamera_core_hwi.so', 'hdr_process', '_Z11hdr_processPvS_', [0x10, 0x140]),
    ('libcamera_core_hwi.so', 'facebeauty_process', '_Z18facebeauty_processPvS_', [0x2cc]),
    ('libcamera_core_hwi.so', 'bokeh_process', '_Z13bokeh_processPvS_', [0x280, 0x28c]),
    ('libcamera_core_hwi.so', 'superphoto_Process', '_Z18superphoto_ProcessPvS_',
     [0x28c, 0x298, 0x2a0, 0x2cc]),
    ('libcamera_core_hwi.so', 'blurless_Process', '_Z16blurless_ProcessPvS_',
     [0x280, 0x28c, 0x2a8, 0x2cc]),
    ('libcamera_core_hwi.so', 'oppo_wrapper_process', '_Z20oppo_wrapper_processPvS_x',
     [0x2cc, 0x2d4]),
]


def function_text(image, name):
    syms = [s for s in image.syms if s['name'] == name and s['value'] and s['size']]
    if not syms:
        raise SystemExit('symbol not found: %s' % name)
    dis = Disassembler(image)
    return ' '.join('%s %s' % (r['mnemonic'], r['op_str'])
                    for r in dis.disasm(syms[0]['value'], syms[0]['size']))


def immediates(text):
    return {int(v, 16) for v in re.findall(r'#(0x[0-9a-f]+)', text)}


def _checks(stock):
    out = []
    img = Image(stock)
    data = open(stock, 'rb').read()
    out.append(('stock HAL1 size', len(data) == EXPECTED['size'], len(data)))
    import hashlib
    out.append(('stock HAL1 sha256',
                hashlib.sha256(data).hexdigest() == EXPECTED['sha256'], ''))
    note = img.section('.note.gnu.build-id')
    build_id = note.data()[16:].hex() if note is not None else ''
    out.append(('stock HAL1 build-id', build_id == EXPECTED['build_id'], build_id, ''))

    # The vendor HAL imports the platform entry points directly (no dlopen).
    imports = {s['name'] for s in img.syms if s['shndx'] == 'SHN_UNDEF'}
    for entry in ('_Z18oppo_platform_initPvyPyS0_S0_', '_Z21oppo_platform_processPvS_y',
                  '_Z20oppo_platform_uninitPvy', '_Z30oppo_platform_updateparametersPvS_y'):
        out.append(('stock HAL1 imports %s' % entry, entry in imports, ''))

    preview = function_text(img, '_ZN7qcamera25QCamera2HardwareInterface11algoPreviewEP21mm_camera_super_buf_tP17mm_camera_buf_defPNS_13QCameraStreamEPvy')
    for off, _ in PREVIEW_PARAM_WRITES:
        out.append(('algoPreview writes param base 0x%x' % off,
                    '#0x%x' % off in preview))
    for off, _ in PREVIEW_CTX_WRITES:
        out.append(('algoPreview writes ctx base 0x%x' % off,
                    '#0x%x' % off in preview))
    out.append(('algoPreview calls oppo_platform_process',
                '_Z21oppo_platform_processPvS_y' in function_calls(img, _ALGO_PREVIEW)))
    out.append(('algoPreview context base 0x%x' % CTX_BASE, '#0x%x' % CTX_BASE in preview))
    out.append(('algoPreview param base 0x%x' % PARAM_BASE, '#0x%x' % PARAM_BASE in preview))

    reset = function_text(img, _ALGO_RESET)
    out.append(('algoReset calls oppo_platform_uninit x3',
                function_calls(img, _ALGO_RESET).count('_Z20oppo_platform_uninitPvy') == 3))
    out.append(('algoReset memclr(param, 0x12EC) x3', reset.count('#0x12ec') >= 3))
    out.append(('algoReset param base 0x1718', '#0x1718' in reset, ''))

    bokeh = function_text(img, _BOKEH_PROCESS)
    out.append(('bokeh process calls oppo_platform_process',
                '_Z21oppo_platform_processPvS_y' in function_calls(img, _BOKEH_PROCESS)))
    out.append(('bokeh ctx base 0x1360', '#0x1360' in bokeh, ''))
    out.append(('bokeh param base 0x70', '#0x70' in bokeh, ''))
    out.append(('bokeh process mask 0x860 (facebeauty|bokeh|picbest)', '#0x860' in bokeh, ''))
    out.append(('bokeh process mask 0x60 (facebeauty|bokeh)', '#0x60' in bokeh, ''))

    bsetup = function_text(img, '_ZN7qcamera17OppoImageBokehInt16setup_ocam_paramEv')
    out.append(('bokeh setup_ocam_param writes param+0x10c0 (#0x1130)', '#0x1130' in bsetup, ''))
    out.append(('bokeh setup_ocam_param writes param+0x10c4 (#0x1134)', '#0x1134' in bsetup, ''))
    out.append(('bokeh setup_ocam_param copies path to param+0x60 (this+0xd0)', '#0xd0' in bsetup, ''))

    # core lib: what the algorithms read out of the frame ctx
    libdir = os.path.join(os.path.dirname(stock), '..')
    core_path = os.path.join(os.path.dirname(stock), '..', 'libcamera_core_hwi.so')
    if os.path.exists(core_path):
        core = Image(core_path)
        for _, label, name, offsets in CORE_CTX_CONSUMERS:
            text = function_text(core, name)
            for off in offsets:
                out.append(('core %s reads ctx+0x%x' % (label, off),
                            '#0x%x' % off in text, ''))
    return out


def main(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument('--stock-hal1', default=DEFAULT_STOCK)
    parser.add_argument('--verify', action='store_true')
    args = parser.parse_args(argv[1:])
    results = _checks(args.stock_hal1)

    if args.verify:
        failed = 0
        for item in results:
            if not item[1]:
                print('FAIL %s' % item[0])
                failed += 1
        print('%d checks, %d failed' % (len(results), failed))
        return 1 if failed else 0

    print('# stock HAL1 module: %s' % args.stock_hal1)
    print('# size=%d sha256=%s build-id=%s' % (
        EXPECTED['size'], EXPECTED['sha256'], EXPECTED['build_id']))
    print('\n## struct bases (C)')
    print('  QCamera2HardwareInterface: param_platform_t = this+0x%x, frame ctx = this+0x%x'
          % (PARAM_BASE, CTX_BASE))
    print('  OppoImageBokehInt:        param_platform_t = this+0x%x, frame ctx = this+0x%x'
          % (BOKEH_PARAM_BASE, BOKEH_CTX_BASE))
    print('  buffer_def_t[10] @ bokeh this+0x%x stride 0x%x'
          % (BOKEH_BUFDEF_BASE, BOKEH_BUFDEF_STRIDE))
    print('\n## algoPreview param writes (this-relative -> public offset)')
    for off, desc in PREVIEW_PARAM_WRITES:
        value = off - PARAM_BASE
        print('  0x%08x  %-46s  -> 0x%03x' % (off, desc, value))
    print('\n## algoPreview frame-ctx writes (this-relative -> public offset)')
    for off, desc in PREVIEW_CTX_WRITES:
        value = off - CTX_BASE
        print('  0x%08x  %-46s  -> 0x%03x' % (off, desc, value))
    print('\n## core-lib ctx consumers (first arg = frame ctx)')
    for lib, label, name, offsets in CORE_CTX_CONSUMERS:
        print('  %-24s %s' % (label, ', '.join('0x%x' % o for o in offsets)))
    print('\n## assertions')
    for item in results:
        print('  %s %s' % ('PASS' if item[1] else 'FAIL', item[0]))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
