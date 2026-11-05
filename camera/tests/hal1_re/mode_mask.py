#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Original "mode -> algorithm mask" selection point (stock OPPO HAL1).

Task-55 established that the algorithm libraries never choose a mode: they take
a 64-bit mask from the caller.  The caller is the stock HAL1 module
`vendor/lib/hw/camera.sdm660.so`; two functions select the mask:

  QCamera2HardwareInterface::OppoPlatformPreviewAlgoInit(void*)   @0xac980
      builds the *preview* mask from getOppoCapMode()/position/4k2k/properties,
      calls oppo_platform_init(this+0x1718, mask|bit63, NULL, &out2, NULL) and
      stores the returned frame mask into this+0x2d18 / this+0x2d1c.
      algoPreview() later ANDs its per-frame mask with that member.

  QCamera2HardwareInterface::algoAfterCPP(...)                   @0x12b5ec
      snapshot / after-CPP path: init @0x12c704 + process @0x12c804/0x12c84a
      using the accumulators this+0x5968/0x596c (mode masks set at capture-mode
      configuration time).  Which algorithm bits those accumulators carry is
      not fully recovered -> listed as a gap.

OPPO capture-mode enum (QCameraParameters::OPPO_CAP_MODES_MAP @0x19bcc0,
15 (string, value) pairs in .rodata):
   1 common, 2 portrait, 3 doubleexposure, 5 filter, 6 gif,
   9 professional, 10 qrcode, 11 commonVideo, 12 fastVideo, 13 slowVideo,
   14 sticker, 15 oar   (4/7/8 have no string in the table)

Usage:
  ./mode_mask.py [--stock-hal1 PATH]            # print the recovered table
  ./mode_mask.py [--stock-hal1 PATH] --verify   # assertions (exit 1 on mismatch)
"""

import argparse
import sys

from hal1re import Image, Disassembler, function_calls

DEFAULT_STOCK = '/home/cy/Project/R11/stock/R11/vendor/lib/hw/camera.sdm660.so'
CAP_MODES_OFFSET = 0x19bcc0
CAP_MODE_COUNT = 15
CAP_MODE_NAMES = {
    1: 'common', 2: 'portrait', 3: 'doubleexposure', 5: 'filter', 6: 'gif',
    9: 'professional', 10: 'qrcode', 11: 'commonVideo', 12: 'fastVideo',
    13: 'slowVideo', 14: 'sticker', 15: 'oar',
}

# algorithm bits used by the preview init path (see task-55 mask table)
ALGO = {
    0x400: 'asd (bit10)',
    0x200: 'vdscesprocess/SCES (bit9)',
    0x100: 'arc_facebeauty (bit8)',
    0x1000: 'imageadjust (bit12)',
    0x2000: 'portraitLightPreview (bit13)',
    0x8000: 'slam (bit15)',
}
PREVIEW_BITS = tuple(ALGO)

PREVIEW_INIT_FUNC = ('_ZN7qcamera25QCamera2HardwareInterface'
                     '27OppoPlatformPreviewAlgoInitEPv')
ALGO_AFTER_CPP = '_ZN7qcamera25QCamera2HardwareInterface12algoAfterCPPEP21mm_camera_super_buf_tPiP17mm_camera_buf_def'
ALGO_PREVIEW = ('_ZN7qcamera25QCamera2HardwareInterface11algoPreview'
                'EP21mm_camera_super_buf_tP17mm_camera_buf_defPNS_13QCameraStreamEPvy')


def function_text(image, name):
    syms = [s for s in image.syms if s['name'] == name and s['value'] and s['size']]
    if not syms:
        raise SystemExit('symbol not found: %s' % name)
    dis = Disassembler(image)
    return ' '.join('%s %s' % (r['mnemonic'], r['op_str'])
                    for r in dis.disasm(syms[0]['value'], syms[0]['size']))


def read_cap_modes(image):
    modes = []
    for i in range(CAP_MODE_COUNT):
        base = CAP_MODES_OFFSET + i * 8
        ptr = image.u32(base)
        value = image.u32(base + 4)
        name = ''
        if ptr:
            raw = image.try_read(ptr, 48)
            if raw:
                name = raw.split(b'\x00')[0].decode('ascii', 'replace')
        modes.append((name, value))
    return modes


def _checks(stock):
    out = []
    img = Image(stock)
    modes = read_cap_modes(img)
    actual = {value: name for name, value in modes}
    for value, name in CAP_MODE_NAMES.items():
        out.append(('cap mode enum %d == %r' % (value, name),
                    actual.get(value) == name, actual.get(value)))

    preview_init = function_text(img, PREVIEW_INIT_FUNC)
    init_calls = function_calls(img, PREVIEW_INIT_FUNC)
    out.append(('OppoPlatformPreviewAlgoInit calls oppo_platform_init',
                '_Z18oppo_platform_initPvyPyS0_S0_' in init_calls))
    out.append(('preview init passes param this+0x1718', '#0x1718' in preview_init))
    out.append(('preview init sets in-process bit 0x80000000',
                '0x80000000' in preview_init))
    out.append(('preview init stores frame mask to this+0x2d18',
                '#0x2d18' in preview_init and '#0x2d1c' in preview_init))
    for bit, desc in ALGO.items():
        out.append(('preview init ORs %s' % desc, '#0x%x' % bit in preview_init))
    out.append(('preview init queries getOppoCapMode',
                any('getOppoCapModeEv' in n for n in init_calls)))
    out.append(('preview init queries isOppoApp',
                any('isOppoAppEv' in n for n in init_calls)))
    out.append(('preview init queries is4k2kVideoResolution',
                any('is4k2kVideoResolutionEv' in n for n in init_calls)))
    out.append(('preview init queries isOARMode',
                any('isOARModeEv' in n for n in init_calls)))

    preview = function_text(img, ALGO_PREVIEW)
    out.append(('algoPreview ANDs mask with member this+0x2d18/0x2d1c',
                '#0x2d18' in preview and '#0x2d1c' in preview))
    out.append(('algoPreview ORs extra bits per frame',
                all('#0x%x' % b in preview for b in (0x200, 0x100, 0x1000))))

    after = function_text(img, ALGO_AFTER_CPP)
    after_calls = function_calls(img, ALGO_AFTER_CPP)
    out.append(('algoAfterCPP calls oppo_platform_init',
                '_Z18oppo_platform_initPvyPyS0_S0_' in after_calls))
    out.append(('algoAfterCPP calls oppo_platform_process',
                after_calls.count('_Z21oppo_platform_processPvS_y') >= 2))
    out.append(('algoAfterCPP uses this+0x5968/0x596c mask accumulators',
                '#0x5968' in after and '#0x596c' in after))
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

    img = Image(args.stock_hal1)
    print('# OPPO capture modes (QCameraParameters::OPPO_CAP_MODES_MAP @0x%x)'
          % CAP_MODES_OFFSET)
    for name, value in read_cap_modes(img):
        print('  enum %2d  %s' % (value, name or '(no string in table)'))
    print('\n# preview mask bits selected by OppoPlatformPreviewAlgoInit:')
    for bit, desc in ALGO.items():
        print('  0x%04x  %s' % (bit, desc))
    print('  bit63 0x80000000 = in-process backend (always set on init)')
    print('\n# preview mask logic (C, from instruction flow):')
    print('  base = (this[0x5b06] ? 0 : 0x400 asd)')
    print('  capMode 13 (slowVideo): if this[0x6300]==2 and position==0 -> base=asd only, in-process')
    print('  capMode 1 (common) and position==0 -> |= 0x200 (SCES)')
    print('  unless (capMode in {1,4,14} or commonVideo+4k2k or isOppoApp) -> |= 0x100 (arc)')
    print('  capMode 1 (common) or commonVideo+4k2k -> |= 0x1000 (imageadjust)')
    print('  property gate and capMode 2 (portrait) -> |= 0x2000 (portraitLight)')
    print('  isOARMode -> in-process bit + |= 0x8000 (slam)')
    print('  init(param=this+0x1718, mask|bit63, NULL, &out2, NULL); store out2 -> this+0x2d18')
    print('\n## assertions')
    for item in results:
        print('  %s %s' % ('PASS' if item[1] else 'FAIL', item[0]))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
