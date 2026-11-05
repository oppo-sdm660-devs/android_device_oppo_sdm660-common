#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""HAL1 RE batch 5: beauty param model path / strength mapping / arc / SCES / FdSelect.

Findings (stock HAL1 module unless noted):
  fillStaticAlgoInfo(this, param) @0x129ad0, 512 B, called from startPreview:
    - param+0x000/0x004 = stream w/h; +0x008 = aligned stride; +0x00c = aligned slice;
      +0x01c = 1 (same as algoPreview, batch 2)
    - param+0x158 / +0x15c = defaults loaded through module globals (GOT 0x19ca10-family)
    - param+0x168 (0x1880) u8 isFront, +0x169 (0x1881) u8 from camera position/capability
    - param+0x060 char[0x80] = formatted path via helper 0x6f2b0 (r1=0x80, r2=0x20,
      format string PC-relative) -> default model/path seed (context: /data/vendor/camera)
    - param+0x109c u32 / +0x10a8 u8 / +0x10b8 u32 / +0x10d8/+0x10dc / +0x10e8 u8 = property
      driven debug/config fields (C offsets)
  fillOppoFBInfo(metadata_param_t* meta) @0x129d80, 576 B (beauty vendor metadata):
    - meta+0x140 u32 = getFaceBeautyLevel(); meta+0x144 u8 camera position;
      meta+0x145/146/147 u8 cap-mode bytes; meta+0x148 isDocFBMode; meta+0x149 is3expHDR;
      meta+0x14a isOppoApp
    - meta+0x218/+0x240/+0x268/+0x290 (stride 0x28) and meta+0x2e0 float, meta+0x360
    - calls getOtherAppVideoBeauty / isVideoBeauty
  platform arc_facebeauty_process_hwi @0x4a30 (592 B):
    param+0x164 -> arc+0x10 (strength)
    param+0x060 -> arc+0x18 (model path, copied via 0x49f0)
    param+0x168 -> arc+0x9e (isFront)
    param+0x16e -> arc+0x9d ; +0x169 -> +0x98 ; +0x16a -> +0x9a ;
    +0x16b -> +0x99 ; +0x16c -> +0x9b ; +0x16d -> +0x9c
  platform vdscesprocess_process_hwi @0x5668 (64 B):
    reads param+0x10bc u32 and pushes it via ocam_algo_updateparameters(size=4, idx=0x200)
  core get_curve_value @0x1fd20 (84 B):
    width = find_biggest_face_width(face); s0=1.0f, s2=2.0f;
    if portrait_mode==1 -> 2.0; if width>=150 -> 2.0; returns 1.0 or 2.0
    => count=0 (width 0) returns 1.0 (default curve), no division/crash.

Usage:
  ./beauty_params.py [--stock-hal1 PATH] [--vendor-lib-dir DIR]     # print tables
  ./beauty_params.py [...] --verify                                 # assertions
"""

import argparse
import os
import sys

from hal1re import Image, function_calls

DEFAULT_STOCK = '/home/cy/Project/R11/stock/R11/vendor/lib/hw/camera.sdm660.so'
DEFAULT_VENDOR = ('/home/cy/Project/R11/proprietary_vendor_oppo_sdm660-common/'
                  'proprietary/vendor/lib')

FSAI = '_ZN7qcamera25QCamera2HardwareInterface18fillStaticAlgoInfoEPNS_20platform_algo_list_tE'
FBINFO = '_ZN7qcamera25QCamera2HardwareInterface14fillOppoFBInfoEP16metadata_param_t'
P = '/home/cy/Project/R11/proprietary_vendor_oppo_sdm660-common/proprietary/vendor/lib/'
PLAT = 'liboppo_platform_hwi.so'
CORE = 'libcamera_core_hwi.so'
ARC = '_Z26arc_facebeauty_process_hwiPvS_y'
VDSCES = '_Z25vdscesprocess_process_hwiPvS_y'

# param field -> (arc struct field, label)
ARC_MAP = [
    (0x164, 0x10, 'strength'),
    (0x168, 0x9e, 'isFront'),
    (0x169, 0x98, 'flag+0x169'),
    (0x16a, 0x9a, 'flag+0x16a'),
    (0x16b, 0x99, 'flag+0x16b'),
    (0x16c, 0x9b, 'flag+0x16c'),
    (0x16d, 0x9c, 'flag+0x16d'),
    (0x16e, 0x9d, 'flag+0x16e'),
]
FBINFO_FIELDS = [
    (0x140, 'getFaceBeautyLevel -> meta+0x140'),
    (0x144, 'camera position byte'),
    (0x145, 'cap-mode byte'),
    (0x146, 'cap-mode byte'),
    (0x147, 'cap-mode byte'),
    (0x148, 'isDocFBMode'),
    (0x149, 'is3expHDREnable'),
    (0x14a, 'isOppoApp'),
]
PROP_FIELDS = [0x109c, 0x10a8, 0x10b8, 0x10d8, 0x10dc, 0x10e8]
MODEL_PATH_CANDIDATES = [
    '/data/vendor/camera',
    '/vendor/lib/libarcsoft_beautyshot_lite_image.so',
    '/etc/camera/skinSeg.model',
    '/etc/camera/Charming.cng',
    '/etc/camera/faceTpl.rgb32',
]


def text(image, name):
    from hal1re import Disassembler
    syms = [s for s in image.syms if s['name'] == name and s['value'] and s['size']]
    if not syms:
        raise SystemExit('symbol not found: %s' % name)
    return ' '.join('%s %s' % (r['mnemonic'], r['op_str'])
                    for r in Disassembler(image).disasm(syms[0]['value'], syms[0]['size']))


def _checks(stock, vendor_dir):
    out = []
    img = Image(stock)

    fsai = text(img, FSAI)
    out.append(('fillStaticAlgoInfo stores param+0x158 default', '#0x158' in fsai, ''))
    out.append(('fillStaticAlgoInfo stores param+0x15c default', '#0x15c' in fsai, ''))
    out.append(('fillStaticAlgoInfo seeds param+0x60 path buffer (+0x60, len 0x80)',
                '#0x60' in fsai and '#0x80' in fsai, ''))
    out.append(('fillStaticAlgoInfo sets param+0x168 isFront (0x1880)',
                '#0x1880' in fsai, ''))
    out.append(('fillStaticAlgoInfo sets param+0x169 (0x1881)', '#0x1881' in fsai, ''))
    out.append(('fillStaticAlgoInfo writes geometry 0x1718/0x171c/0x1720/0x1724/0x1734',
                all(v in fsai for v in ('#0x1718', '#0x171c', '#0x1720', '#0x1724', '#0x1734')), ''))
    for off in PROP_FIELDS:
        out.append(('fillStaticAlgoInfo property field param+0x%x' % off,
                    ('#0x%x' % off) in fsai, ''))
    out.append(('fillStaticAlgoInfo called from startPreview',
                '_ZN7qcamera25QCamera2HardwareInterface18fillStaticAlgoInfoEPNS_20platform_algo_list_tE'
                in function_calls(img, '_ZN7qcamera25QCamera2HardwareInterface12startPreviewEv'), ''))

    fbinfo = text(img, FBINFO)
    for off, label in FBINFO_FIELDS:
        out.append(('fillOppoFBInfo %s' % label, ('0x%x' % off) in fbinfo or
                    ('0x140' in fbinfo and off == 0x140), ''))
    out.append(('fillOppoFBInfo calls getFaceBeautyLevel',
                any('getFaceBeautyLevel' in c for c in function_calls(img, FBINFO)), ''))
    out.append(('fillOppoFBInfo calls isDocFBMode/is3expHDREnable/isOppoApp/isVideoBeauty',
                all(any(x in c for c in function_calls(img, FBINFO))
                    for x in ('isDocFBMode', 'is3expHDREnable', 'isOppoApp', 'isVideoBeauty')), ''))
    for v in (0x218, 0x240, 0x268, 0x290):
        out.append(('fillOppoFBInfo vendor array meta+0x%x (stride 0x28)' % v,
                    ('#0x%x' % v) in fbinfo, ''))

    plat_path = os.path.join(vendor_dir, PLAT)
    core_path = os.path.join(vendor_dir, CORE)
    if os.path.exists(plat_path):
        plat = Image(plat_path)
        arc = text(plat, ARC)
        for poff, aoff, label in ARC_MAP:
            out.append(('arc hook param+0x%03x -> arc+0x%02x (%s)' % (poff, aoff, label),
                        ('#0x%x' % poff) in arc and ('#0x%x' % aoff) in arc, ''))
        out.append(('arc hook copies param+0x60 model path to arc+0x18',
                    '#0x60' in arc and '#0x18' in arc and '0x49f0' in arc, ''))
        vs = text(plat, VDSCES)
        out.append(('SCES hook reads param+0x10bc', '#0x10bc' in vs, ''))
        out.append(('SCES hook pushes size 4 / index 0x200',
                    ('#4' in vs or 'movs r1, #4' in vs) and '#0x200' in vs and
                    any('ocam_algo_updateparameters' in c for c in function_calls(plat, VDSCES)), ''))
    if os.path.exists(core_path):
        core = Image(core_path)
        cv = text(core, '_Z15get_curve_valueP20platform_face_data_t15portrait_mode_t')
        out.append(('get_curve_value threshold 150 (0x96)', '#0x96' in cv, ''))
        out.append(('get_curve_value constants 2.0 and portrait branch',
                    'vmov.f32 s2, #2.000000e+00' in cv and '#0x1' in cv, ''))
        out.append(('get_curve_value divides by nothing (no vdiv in body)', 'vdiv' not in cv, ''))
        # model path candidate strings in core
        core_strings = b''.join(s.data() for s in core._sections
                                if s['sh_type'] != 'SHT_NOBITS')
        found = [p for p in MODEL_PATH_CANDIDATES
                 if p.encode() in core_strings or p.split('/')[-1].encode() in core_strings]
        out.append(('core ships sensetime model path strings (%d candidates)' % len(found),
                    len(found) >= 2, ''))
    return out


def main(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument('--stock-hal1', default=DEFAULT_STOCK)
    parser.add_argument('--vendor-lib-dir', default=DEFAULT_VENDOR)
    parser.add_argument('--verify', action='store_true')
    args = parser.parse_args(argv[1:])
    results = _checks(args.stock_hal1, args.vendor_lib_dir)

    if args.verify:
        failed = 0
        for item in results:
            if not item[1]:
                print('FAIL %s' % item[0])
                failed += 1
        print('%d checks, %d failed' % (len(results), failed))
        return 1 if failed else 0

    print('# param fields seeded at startPreview by fillStaticAlgoInfo')
    print('  param+0x000/0x004 w/h; +0x008 aligned stride; +0x00c aligned slice; +0x01c=1')
    print('  param+0x158/+0x15c defaults via module globals; +0x60 path buffer (len 0x80)')
    print('  param+0x168 isFront; +0x169 capability byte; property fields 0x109c..0x10e8')
    print()
    print('# vendor beauty metadata fillOppoFBInfo(metadata_param_t*)')
    for off, label in FBINFO_FIELDS:
        print('  meta+0x%03x  %s' % (off, label))
    print()
    print('# front camera: platform arc_facebeauty_process_hwi mapping')
    print('  arc+0x10 <- param+0x164 strength ; arc+0x18 <- param+0x60 model path')
    for poff, aoff, label in ARC_MAP[1:]:
        print('  arc+0x%02x <- param+0x%03x (%s)' % (aoff, poff, label))
    print()
    print('# SCES extra param: param+0x10bc u32 (size 4, idx 0x200)')
    print('# bokeh count=0: width 0 -> get_curve_value returns 1.0 (default), no crash')
    print()
    print('# model path candidates seen in OPPO libraries')
    for p in MODEL_PATH_CANDIDATES:
        print('  %s' % p)
    print()
    print('## assertions')
    for item in results:
        print('  %s %s' % ('PASS' if item[1] else 'FAIL', item[0]))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
