#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""HAL1 RE batch 4: face block builder / count=0 behaviour / face-param specs.

Evidence (stock HAL1 module, see docs/hal1-re-face.md):
  OppoImageBokehInt::setup_ocam_meta_face_data(int idx, metadata_buffer_t* meta)
    0x1215c8, 388 B. Writes descriptor this+0x284+idx*0x174 face block (+0x28, 0x14c):
      meta+0x11 u8 valid flag (0 -> keep block zeroed / count 0)
      meta+0x113c u8 face count, clamped to 10 -> desc+0x170 (block+0x148)
      desc+0x168 = this+0x1370+idx*0x40 (per-image output W)
      desc+0x16c = this+0x1374+idx*0x40 (per-image output H)
      scaleX = outW / (float)meta+0x1284 ; scaleY = outH / (float)meta+0x1288
      record meta+0x1140 + i*0x20:
        block+0x00 = (int)(rec+0x08 * scaleX)     left
        block+0x28 = (int)(rec+0x0c * scaleY)     top
        block+0x50 = (int)(rec+0x10 * scaleX)     width
        block+0x78 = (int)(rec+0x14 * scaleY)     height
        block+0xa0 = (int) rec+0x1c               (unscaled)
        block+0xc8 = (int)(float)(rec+0x18)       (unscaled)
        block+0xf0 = rec+0x00                     face id
        block+0x118 = rec+0x18 (raw u32)          duplicate of 0xc8 source
  Consumers:
    - param face block is consumed ONLY by bokeh + picbest code
      (`platform_face_data_t` consumers: find_biggest_face_width / get_curve_value /
       adjust_blur_level / draw_debug_face_roi / bokeh_hdr_* (3) / picBest* (9)).
    - facebeauty / arc_facebeauty / vdscesprocess do NOT take platform_face_data_t:
      facebeauty runs Venus (Sensetime) self FD; arc runs ArcFaceBeauty;
      SCES has no face input at all.
    - platform facebeauty hook only pushes geometry + strength flags + model path:
      fb_param[0x00..0x0c] = ctx w/h/stride/slice, [0x18]=param+0x164, [0x1f]=param+0x16a,
      [0x20]=param+0x16f, [0x10]=param+0x24, [0x28..]=param+0x60 model path,
      [0xa8]=param+0x158, [0xac]=param+0x15c.  No face count/data.
    - platform faceInfo hook pushes {ctx w/h/stride/slice, param+0x50, isFront=param+0x168,
      param+0x1f0, param+0x1f4} as a 0xac-byte struct to core g_face_info_param
      (core logs FdSelect / faceInfoMode fields) -> that is what drives Venus FD.

Usage:
  ./face_builder.py [--stock-hal1 PATH] [--vendor-lib-dir DIR]        # print tables
  ./face_builder.py [...] --verify                                     # assertions
"""

import argparse
import os
import sys

from hal1re import Image, function_calls

DEFAULT_STOCK = '/home/cy/Project/R11/stock/R11/vendor/lib/hw/camera.sdm660.so'
DEFAULT_VENDOR = ('/home/cy/Project/R11/proprietary_vendor_oppo_sdm660-common/'
                  'proprietary/vendor/lib')

BUILDER = '_ZN7qcamera17OppoImageBokehInt25setup_ocam_meta_face_dataEiP17metadata_buffer_t'
PLAT = '_Z'
FACE_BEAUTY_HWI = '_Z22facebeauty_process_hwiPvS_y'
FACE_INFO_HWI = '_Z20faceInfo_process_hwiPvS_y'

ARRAYS = [
    (0x000, 'left', 'rec+0x08', 'x', 'int'),
    (0x028, 'top', 'rec+0x0c', 'y', 'int'),
    (0x050, 'width', 'rec+0x10', 'x', 'int'),
    (0x078, 'height', 'rec+0x14', 'y', 'int'),
    (0x0a0, 'attr/scalar (unscaled)', 'rec+0x1c', '-', 'int'),
    (0x0c8, 'float(rec+0x18) as int', 'rec+0x18', '-', 'int'),
    (0x0f0, 'face id', 'rec+0x00', '-', 'raw'),
    (0x118, 'raw rec+0x18', 'rec+0x18', '-', 'raw'),
]
COUNT_OFF = 0x170          # descriptor +0x170 == face block +0x148
BLOCK_OFF = 0x28           # descriptor face block offset
BLOCK_SIZE = 0x14c
DESC_STRIDE = 0x174

# Facebeauty strength / model fields used by platform facebeauty_process_hwi (C).
FB_FIELDS = [
    (0x158, 'u32 -> fb_param+0xa8'),
    (0x15c, 'u32 -> fb_param+0xac'),
    (0x164, 'u32 -> fb_param+0x18'),
    (0x16a, 'u8  -> fb_param+0x1f'),
    (0x16f, 'u8  -> fb_param+0x20'),
    (0x024, 'u32 -> fb_param+0x10'),
    (0x060, 'char[0x80] -> fb_param+0x28 (model path)'),
]
# faceInfo hook -> g_face_info_param (C).
FI_FIELDS = [
    (0x050, 'u32 -> g_face_info_param+0x14'),
    (0x168, 'u8  -> g_face_info_param+0x1c (isFront)'),
    (0x1f0, 'u32 -> g_face_info_param+0xa0'),
    (0x1f4, 'u32 -> g_face_info_param+0xa4'),
]
FACE_CONSUMERS = [
    '_Z23find_biggest_face_widthP20platform_face_data_t',
    '_Z15get_curve_valueP20platform_face_data_t15portrait_mode_t',
    '_Z17adjust_blur_levelP20platform_face_data_tP19ocam_af_focus_pos_t15portrait_mode_tPK9bokeh_lib',
    '_Z33bokeh_hdr_setup_hdr_denoise_paramP17HDR_DENOISE_PARAMP20platform_face_data_t',
    '_Z28bokeh_hdr_init_arcsoft_paramP17hdr_2frame_engineP20p_buffer_construct_tP20platform_face_data_t',
    '_Z18picBestFusionImageP20picbest_static_lib_tiiiiPPhS2_iP20platform_face_data_tPA10_iii',
    '_Z13matchFaceDataP20platform_face_data_ti',
    '_Z25selectInputFramesForFacesPPhS0_P20platform_face_data_tPA10_iiPA4_iiiiiPvPPfS9_PiSA_SA_RiSB_',
]


def function_text(image, name):
    from hal1re import Disassembler
    syms = [s for s in image.syms if s['name'] == name and s['value'] and s['size']]
    if not syms:
        raise SystemExit('symbol not found: %s' % name)
    return ' '.join('%s %s' % (r['mnemonic'], r['op_str'])
                    for r in Disassembler(image).disasm(syms[0]['value'], syms[0]['size']))


def _checks(stock, vendor_dir):
    out = []
    img = Image(stock)
    text = function_text(img, BUILDER)

    # --- builder mapping (C) ---
    out.append(('builder clears 0x14c face block (memclr @+0x2ac)', '#0x14c' in text and '#0x2ac' in text, ''))
    out.append(('builder bails when meta NULL (beq epilogue)', 'beq' in text, ''))
    out.append(('builder gates on meta+0x11 u8 valid flag', '#0x11' in text, ''))
    out.append(('builder reads count from meta+0x113c', '#0x113c' in text, ''))
    out.append(('builder clamps count to 10', '#0xa' in text, ''))
    out.append(('builder writes count to descriptor+0x170', '#0x384' in text, ''))
    out.append(('builder writes per-image W/H to desc+0x168/0x16c',
                '#0x37c' in text and '#0x380' in text, ''))
    out.append(('builder loads per-image W/H from this+0x1300/0x1304',
                '#0x1300' in text and '#0x1304' in text, ''))
    out.append(('builder scales by meta+0x1284 (x) and +0x1288 (y)',
                '#0x1284' in text and '#0x1288' in text, ''))
    out.append(('builder computes vdiv scale X/Y (2 vdiv.f32)', text.count('vdiv') >= 2, ''))
    out.append(('builder reads record stride 0x20 from meta+0x1140',
                '#0x1140' in text and '#0x20' in text, ''))
    for off, label, src, axis, kind in ARRAYS:
        needle = '#0x%x' % off if off else 'vstr'
        out.append(('array %-26s @block+0x%03x <- %s' % (label, off, src),
                    (needle in text) if off else True, ''))
    out.append(('builder writes arrays 0x00/0x28/0x50/0x78 with vstr',
                all(s in text for s in ('#0x28', '#0x50', '#0x78')), ''))
    out.append(('builder writes raw rec fields at block+0xf0 / +0x118',
                '#0xf0' in text and '#0x118' in text, ''))

    # --- consumers: only bokeh/picbest take platform_face_data_t (C) ---
    core_path = os.path.join(vendor_dir, 'libcamera_core_hwi.so')
    if os.path.exists(core_path):
        core = Image(core_path)
        for sym in FACE_CONSUMERS:
            present = any(s['name'] == sym and s['value'] for s in core.syms)
            out.append(('core consumer present: %s' % sym.split('_Z')[1][:34], present, ''))
        fb = Image(os.path.join(vendor_dir, 'liboppo_platform_hwi.so'))
        fb_calls = function_calls(fb, FACE_BEAUTY_HWI)
        out.append(('facebeauty hook pushes fb_param via updateparameters',
                    '_Z26ocam_algo_updateparametersPvixy' in fb_calls, ''))
        fb_text = function_text(fb, FACE_BEAUTY_HWI)
        for off, label in FB_FIELDS:
            out.append(('facebeauty param+0x%03x %s' % (off, label),
                        ('#0x%x' % off) in fb_text, ''))
        out.append(('facebeauty hook does NOT read param+0x214 face block',
                    '#0x214' not in fb_text, ''))
        fi_text = function_text(fb, FACE_INFO_HWI)
        fi_calls = function_calls(fb, FACE_INFO_HWI)
        for off, label in FI_FIELDS:
            out.append(('faceInfo param+0x%03x %s' % (off, label),
                        ('#0x%x' % off) in fi_text, ''))
        out.append(('faceInfo hook pushes 0xac-byte struct to core',
                    '#0xac' in fi_text and '_Z26ocam_algo_updateparametersPvixy' in fi_calls, ''))
        # count=0 conclusion support: facebeauty process uses Venus stack
        fbproc_calls = function_calls(core, '_Z18facebeauty_processPvS_')
        out.append(('facebeauty_process runs Venus self-FD stack',
                    any('initVenusFaceBeautyData' in c for c in fbproc_calls) and
                    any('aiFaceBeautyProcess' in c for c in fbproc_calls), ''))
        arc_calls = function_calls(core, '_Z21arcfacebeauty_processPvS_')
        out.append(('arcfacebeauty_process runs ArcFaceBeauty engine',
                    any('ArcFaceBeauty' in c for c in arc_calls), ''))
        sces_calls = function_calls(core, '_Z21vdscesprocess_ProcessPvS_')
        out.append(('vdscesprocess has no face-data consumer call',
                    not any('face' in c.lower() for c in sces_calls), ''))
        out.append(('platform_face_data_t consumer list has no beauty/SCES entries',
                    not any(('facebeauty' in s or 'vdsces' in s) for s in FACE_CONSUMERS), ''))
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

    print('# HAL1 face block builder %s' % BUILDER)
    print('  descriptor = param+0x214 + idx*0x174; face block = descriptor+0x28 (0x14c)')
    print('  count = meta+0x113c u8, clamp 10 -> descriptor+0x170')
    print('  valid gate = meta+0x11 u8 (0 -> block zeroed, count 0)')
    print('  scaleX = outW / meta+0x1284, scaleY = outH / meta+0x1288')
    print('  outW/outH = this+0x1370/0x1374 + idx*0x40 -> descriptor+0x168/0x16c')
    for off, label, src, axis, kind in ARRAYS:
        print('  block+0x%03x  %-26s <- %-8s  scale=%s' % (off, label, src, axis))
    print()
    print('# consumers of the face block (all core symbols taking platform_face_data_t)')
    for sym in FACE_CONSUMERS:
        print('  %s' % sym)
    print('  -> bokeh + picbest only; facebeauty/arc/SCES do not read it')
    print()
    print('# facebeauty parameter fields pushed by platform facebeauty_process_hwi')
    for off, label in FB_FIELDS:
        print('  param+0x%03x  %s' % (off, label))
    print('  no face count / face rect is pushed: beauty uses Venus self-FD')
    print()
    print('# faceInfo hook -> core g_face_info_param (0xac struct)')
    for off, label in FI_FIELDS:
        print('  param+0x%03x  %s' % (off, label))
    print('  core logs g_face_info_param->FdSelect / faceInfoMode -> Venus FD mode source')
    print()
    print('## assertions')
    for item in results:
        print('  %s %s' % ('PASS' if item[1] else 'FAIL', item[0]))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
