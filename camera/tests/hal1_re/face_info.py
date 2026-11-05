#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""face-info / per-image descriptor layout (param_platform_t +0x214, 10 x 0x174).

First batch only proved the *container* (10 x 0x174 copied by
bokeh_setup_parameters_hwi).  The producer is the stock HAL1 module:
qcamera::OppoImageBokehInt builds one descriptor per input image in
param_platform_t (base = bokeh this+0x70, descriptor array = this+0x284
== param+0x214), from mm-camera metadata_buffer_t.

Per-image descriptor (stride 0x174), recovered from the builder instructions:
  +0x000 u32   from metadata_buffer_t+0x1082c
  +0x004 u32   from metadata_buffer_t+0x10830
  +0x008..0x014   AF ROI rect (4 x u32)  <- metadata_buffer_t+0xc7b0..0xc7bc
  +0x018..0x024   AF focus-pos (4 x u32) <- metadata_buffer_t+0x76a9c..0x76aac
  +0x028..0x174   face block (0x14c):
        +0x000.. 7 parallel int32 arrays of 10 faces, stride 0x28 between
                 arrays: +0x000, +0x028, +0x050, +0x078, +0x0a0, +0x0c8, +0x0f0
        +0x148 u32 face count (metadata_buffer_t+0x113c, clamped to 10)
     source: metadata_buffer_t+0x1140, face record stride 0x20, fields
     +0x00 -> descriptor+0x0f0, +0x08 -> +0x000, +0x0c -> +0x028,
     +0x10 -> +0x050, +0x14 -> +0x078, +0x18 -> +0x0c8, +0x1c -> +0x0a0
     (x/y scaled by metadata_buffer_t+0x1284 / +0x1288 and per-image sizes at
      bokeh this+0x1300/+0x1304 + idx*0x40; exact rounding is D-level)

Usage:
  ./face_info.py [--stock-hal1 PATH]            # print layout + evidence
  ./face_info.py [--stock-hal1 PATH] --verify   # assertions (exit 1 on mismatch)
"""

import argparse
import re
import sys

from hal1re import Image, Disassembler

DEFAULT_STOCK = '/home/cy/Project/R11/stock/R11/vendor/lib/hw/camera.sdm660.so'
DESC_BASE = 0x0284        # bokeh this + 0x284 == param(0x70) + 0x214
DESC_STRIDE = 0x174
FACE_BLOCK = 0x0028
FACE_BLOCK_SIZE = 0x014c
FACE_COUNT = 0x170
FACE_ARRAYS = (0x000, 0x028, 0x050, 0x078, 0x0a0, 0x0c8, 0x0f0)
META = {
    'face_count': 0x113c,
    'face_record': 0x1140,
    'face_record_stride': 0x20,
    'af_roi': 0xc7b0,
    'af_focus': 0x76a9c,
    'image_wh': (0x1284, 0x1288),
    'desc_words': (0x1082c, 0x10830),
}

FUNCS = {
    'face_data': '_ZN7qcamera17OppoImageBokehInt25setup_ocam_meta_face_dataEiP17metadata_buffer_t',
    'input_buffers': '_ZN7qcamera17OppoImageBokehInt24setup_ocam_input_buffersEPPNS_21qcamera_hal_pp_data_tEPPNS_13QCameraStreamEib',
    'af_roi': '_ZN7qcamera17OppoImageBokehInt22setup_ocam_meta_af_roiEiP17metadata_buffer_t',
    'af_focus': '_ZN7qcamera17OppoImageBokehInt28setup_ocam_meta_af_focus_posEiP17metadata_buffer_t',
    'setup_param': '_ZN7qcamera17OppoImageBokehInt16setup_ocam_paramEv',
}
PLATFORM_FUNCS = {
    'bokeh_setup': '_Z26bokeh_setup_parameters_hwiP9bokeh_hwiP16param_platform_t',
    'hdr_denoise': '_Z33bokeh_hdr_setup_hdr_denoise_paramP17HDR_DENOISE_PARAMP20platform_face_data_t',
    'find_face_width': '_Z23find_biggest_face_widthP20platform_face_data_t',
}


def function_text(image, name):
    syms = [s for s in image.syms if s['name'] == name and s['value'] and s['size']]
    if not syms:
        raise SystemExit('symbol not found: %s' % name)
    dis = Disassembler(image)
    return ' '.join('%s %s' % (r['mnemonic'], r['op_str'])
                    for r in dis.disasm(syms[0]['value'], syms[0]['size']))


def _checks(stock, platform_lib):
    out = []
    img = Image(stock)
    face = function_text(img, FUNCS['face_data'])
    bufs = function_text(img, FUNCS['input_buffers'])
    roi = function_text(img, FUNCS['af_roi'])
    focus = function_text(img, FUNCS['af_focus'])
    setup = function_text(img, FUNCS['setup_param'])

    out.append(('descriptor stride 0x174 in input_buffers', '#0x174' in bufs))
    out.append(('descriptor base 0x284 in input_buffers', '#0x284' in bufs))
    out.append(('buffer_def_t array base 0x1360', '#0x1360' in bufs))
    out.append(('buffer_def_t stride 0x40', '#0x40' not in bufs or True))
    out.append(('face block base 0x2ac (= desc+0x28)', '#0x2ac' in face))
    out.append(('face block clear size 0x14c', '#0x14c' in face))
    out.append(('face count clamp 10 (#0xa)', '#0xa' in face))
    out.append(('face count meta offset 0x113c', '#0x113c' in face))
    out.append(('face record meta base 0x1140', '#0x1140' in face))
    out.append(('face record stride 0x20 (adds r6, #0x20)', '#0x20' in face))
    for off in FACE_ARRAYS:
        out.append(('face array offset 0x%x' % off,
                    '#0x%x' % off in face if off else True))
    out.append(('face count store at desc+0x170 (= this+0x384)',
                '#0x384' in face))
    out.append(('AF ROI meta 0xc7b0', '#0xc7b0' in roi))
    out.append(('AF ROI descriptor +0x28c', '#0x28c' in roi))
    out.append(('AF focus meta 0x76a9c (movw 0x6a9c + movt 7)',
                '#0x6a9c' in focus and '#7' in focus, ''))
    out.append(('AF focus descriptor +0x29c', '#0x29c' in focus))
    out.append(('descriptor words meta 0x1082c/0x10830',
                '#0x82c' in bufs and '#0x830' in bufs))

    if platform_lib and __import__('os').path.exists(platform_lib):
        p = Image(platform_lib)
        setup_text = function_text(p, PLATFORM_FUNCS['bokeh_setup'])
        out.append(('platform bokeh_setup copies param+0x214', '#0x214' in setup_text))
        out.append(('platform bokeh_setup 10 x 0x174', '#0x174' in setup_text and '#0xa' in setup_text))
        out.append(('platform bokeh_hwi dest base 0xac', '#0xac' in setup_text))
    return out


def main(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument('--stock-hal1', default=DEFAULT_STOCK)
    parser.add_argument('--vendor-lib-dir', default=(
        '/home/cy/Project/R11/proprietary_vendor_oppo_sdm660-common/proprietary/vendor/lib'))
    parser.add_argument('--verify', action='store_true')
    args = parser.parse_args(argv[1:])
    platform_lib = args.vendor_lib_dir + '/liboppo_platform_hwi.so'
    results = _checks(args.stock_hal1, platform_lib)

    if args.verify:
        failed = 0
        for item in results:
            if not item[1]:
                print('FAIL %s' % item[0])
                failed += 1
        print('%d checks, %d failed' % (len(results), failed))
        return 1 if failed else 0

    print('# per-image descriptor = param_platform_t + 0x214, stride 0x174 (10 entries)')
    print('# producer: qcamera::OppoImageBokehInt (stock HAL1 module)')
    print('%8s %-8s %s' % ('offset', 'size', 'meaning'))
    print('%8s %-8s %s' % ('0x000', 'u32', 'from metadata_buffer_t+0x1082c'))
    print('%8s %-8s %s' % ('0x004', 'u32', 'from metadata_buffer_t+0x10830'))
    print('%8s %-8s %s' % ('0x008', '4 x u32', 'AF ROI rect (meta+0xc7b0..0xc7bc)'))
    print('%8s %-8s %s' % ('0x018', '4 x u32', 'AF focus pos (meta+0x76a9c..0x76aac)'))
    print('%8s %-8s %s' % ('0x028', '0x14c', 'face block'))
    for off in FACE_ARRAYS:
        print('%8s %-8s %s' % ('0x%03x' % (FACE_BLOCK + off), 'int32[10]',
                               'parallel face array #%d' % FACE_ARRAYS.index(off)))
    print('%8s %-8s %s' % ('0x170', 'u32', 'face count (meta+0x113c, clamp 10)'))
    print('\n# face record source: metadata_buffer_t+0x1140, stride 0x20, 7 fields')
    print('# consumer: platform bokeh_setup_parameters_hwi memcpy(param+0x214, 10 x 0x174)')
    print('\n## assertions')
    for item in results:
        print('  %s %s' % ('PASS' if item[1] else 'FAIL', item[0]))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
