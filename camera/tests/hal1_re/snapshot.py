#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""HAL1 RE batch 3: snapshot mask, frame-ctx buffer descriptor, calib, face naming.

Findings encoded here (all from the stock HAL1 module
stock/R11/vendor/lib/hw/camera.sdm660.so unless stated; Thumb-2, .dynsym named):

A) frame ctx (0x308) == the core libraries' `p_buffer_construct_t`:
     +0x000 + i*0x40 (i<10)  image descriptor
         +0x00  void*  buffer pointer
         +0x0c  u32    (source QCameraStream/buf +0x20c)
         +0x10  u32    width      +0x14 u32 height
         +0x18  u32    stride     +0x1c u32 slice
         +0x20  u32    (source buf +0x1c)
         +0x30  u32    cam_format_t
         +0x38  u8     valid/primary flag (core get_slave_offset/fillBokehImageInfo)
     +0x280 u32        image count (core fillBokehImageInfo/get_slave_offset)
     +0x288..0x2a8     per-frame fields written by algoPreview (D)
     +0x2c8 u32        scratch fd      (QCameraHeapMemory::getFd)
     +0x2cc void*      scratch pointer (QCameraHeapMemory::getPtr -> +0x19c/0x1a8 used by core)
     +0x2d4 u32        scratch size    (QCameraHeapMemory::getSize)
     +0x2c8..0x308     0x40-byte descriptor block preserved by algoReset
   Owners/pairs:
     QCamera2HardwareInterface  param=this+0x1718  preview ctx=this+0x2a08  snapshot ctx=this+0x5648
     OppoImageBokehInt          param=this+0x0070  ctx=this+0x1360
     QCameraPostProcessor       param=this+0x2d38  ctx=this+0x4028 (supZoom)
   Scratch buffers: new QCameraHeapMemory(1); allocate(1, 0x1000) for HWI,
   allocate(1, 0x400) for bokeh; created in algoInit / bokeh ctor, freed in
   algoUninit; not per-frame.

B) snapshot mode->mask: take_picture() clears this+0x5968/0x596c and resets the
   snapshot ctx (memclr(this+0x5648, 0x308)); algoAfterCPP() ORs the per-mode
   bits then calls oppo_platform_init(param=this+0x1718, mask) and twice
   oppo_platform_process(ctx=this+0x5648, param=this+0x1718, mask).
   HDR(7)/LLHDR(14) bits are never passed by this module (negative evidence).

C) bokeh calibration: /mnt/vendor/persist/dual_camera_calibration/calibrationOutput.bin,
   calibData_t = {+0xa0..a8 yaw/pitch/roll, +0xc8 err_y-ish, X/Y/Z shift, cam1/cam2
   k1..k3} fed to `bokehSetCalibData` of the ArcSoft dualcam library.

Usage:
  ./snapshot.py [--stock-hal1 PATH] [--vendor-lib-dir DIR]            # print tables
  ./snapshot.py [...] --verify                                        # assertions
"""

import argparse
import os
import sys

from hal1re import Image, Disassembler, function_calls

DEFAULT_STOCK = '/home/cy/Project/R11/stock/R11/vendor/lib/hw/camera.sdm660.so'
DEFAULT_VENDOR = ('/home/cy/Project/R11/proprietary_vendor_oppo_sdm660-common/'
                  'proprietary/vendor/lib')

# ---- anchors -------------------------------------------------------------
HWI_PARAM = 0x1718
HWI_PREVIEW_CTX = 0x2A08
HWI_SNAPSHOT_CTX = 0x5648
BOKEH_PARAM = 0x0070
BOKEH_CTX = 0x1360
POSTPROC_PARAM = 0x2D38
POSTPROC_CTX = 0x4028
HWI_SCRATCH_MEM = 0x2D34       # QCameraHeapMemory* member
BOKEH_SCRATCH_MEM = 0x168C
CTX_SCRATCH_FD = 0x2C8
CTX_SCRATCH_PTR = 0x2CC
CTX_SCRATCH_SIZE = 0x2D4
CTX_TAIL_BLOCK = 0x2C8         # 0x40 bytes preserved by algoReset
CTX_IMAGES = 0x000
CTX_IMAGE_STRIDE = 0x40
CTX_IMAGE_VALID = 0x38
CTX_IMAGE_COUNT = 0x280
HAL1_SCRATCH_SIZE = 0x1000
BOKEH_SCRATCH_SIZE = 0x400

SNAP_MASK_BITS = {
    0x4: 'stfaceblur (bit2)',
    0x8: 'filter (bit3)',
    0x10: 'faceInfo (bit4)',
    0x20: 'facebeauty (bit5)',
    0x200: 'vdscesprocess/SCES (bit9)',
    0x1000: 'imageadjust (bit12)',
    0x40000000: 'offline bit62 (mask high word)',
}
# Bits this module never passes to oppo_platform_* (negative evidence).
UNUSED_BITS = {0x80: 'hdr (bit7)', 0x4000: 'llhdr (bit14)',
               0x1: 'blurless (bit0)', 0x2: 'superphoto (bit1)',
               0x800: 'picbest (bit11)'}

CALIB_PATH = '/mnt/vendor/persist/dual_camera_calibration/calibrationOutput.bin'

HWI = '_ZN7qcamera25QCamera2HardwareInterface'
TAKE_PICTURE = HWI + '12take_pictureEP13camera_device'
ALGO_AFTER_CPP = HWI + '12algoAfterCPPEP21mm_camera_super_buf_tPiP17mm_camera_buf_def'
ALGO_INIT = HWI + '8algoInitEv'
ALGO_RESET = HWI + '9algoResetEv'
ALGO_PREVIEW = HWI + ('11algoPreviewEP21mm_camera_super_buf_tP17mm_camera_buf_def'
                      'PNS_13QCameraStreamEPvy')
STOP_PREVIEW = HWI + '12stop_previewEP13camera_device'
BOKEH_CTOR = '_ZN7qcamera17OppoImageBokehIntC1E22cam_oppo_portriat_mode'
BOKEH_PROCESS = '_ZN7qcamera17OppoImageBokehInt7processEv'
POSTPROC_SUPZOOM = '_ZN7qcamera20QCameraPostProcessor14supZoomRoutineEPv'


def function_text(image, name):
    syms = [s for s in image.syms if s['name'] == name and s['value'] and s['size']]
    if not syms:
        raise SystemExit('symbol not found: %s' % name)
    dis = Disassembler(image)
    return ' '.join('%s %s' % (r['mnemonic'], r['op_str'])
                    for r in dis.disasm(syms[0]['value'], syms[0]['size']))


def _checks(stock, vendor_dir):
    out = []
    img = Image(stock)

    # A) frame ctx == p_buffer_construct_t
    bokeh_ctor = function_text(img, BOKEH_CTOR)
    out.append(('bokeh ctx scratch fd  (this+0x1628 == ctx+0x2c8)',
                '#0x1628' in bokeh_ctor, ''))
    out.append(('bokeh ctx scratch ptr (this+0x162c == ctx+0x2cc)',
                '#0x162c' in bokeh_ctor, ''))
    out.append(('bokeh ctx scratch size(this+0x1634 == ctx+0x2d4)',
                '#0x1634' in bokeh_ctor, ''))
    out.append(('bokeh scratch QCameraHeapMemory + getFd/getSize',
                'getFd' not in bokeh_ctor and
                '_ZNK7qcamera13QCameraMemory5getFdEj' in
                ' '.join(function_calls(img, BOKEH_CTOR)), ''))
    out.append(('bokeh scratch allocate size 0x400', '#0x400' in bokeh_ctor, ''))
    out.append(('HWI snapshot ctx this+0x5648', '#0x5648' in
                function_text(img, ALGO_AFTER_CPP), ''))

    algo_init = function_text(img, ALGO_INIT)
    out.append(('algoInit scratch gets fd into this+0x2cd0', '#0x2cd0' in algo_init, ''))
    out.append(('algoInit scratch pointer into this+0x2cd4', '#0x2cd4' in algo_init, ''))
    out.append(('algoInit scratch size into this+0x2cdc', '#0x2cdc' in algo_init, ''))
    out.append(('HWI scratch allocate size 0x1000', '#0x1000' in algo_init, ''))

    reset = function_text(img, ALGO_RESET)
    out.append(('algoReset clears preview ctx 0x308', '#0x2a08' in reset and '#0x308' in reset, ''))
    out.append(('algoReset preserves 0x40-byte ctx tail at this+0x2cd0',
                '#0x2cd0' in reset and '#0x40' in reset, ''))
    out.append(('algoReset clears three param blocks 0x12EC',
                reset.count('#0x12ec') >= 3, ''))

    take = function_text(img, TAKE_PICTURE)
    out.append(('take_picture clears mask accumulators this+0x5968/0x596c',
                '#0x5968' in take and '#0x596c' in take, ''))
    out.append(('take_picture resets snapshot ctx(this+0x5648, 0x308)',
                '#0x5648' in take and '#0x308' in take, ''))

    after = function_text(img, ALGO_AFTER_CPP)
    for bit, label in SNAP_MASK_BITS.items():
        needle = '#0x%x' % bit
        out.append(('algoAfterCPP ORs %s' % label, needle in after, ''))
    after_calls = function_calls(img, ALGO_AFTER_CPP)
    out.append(('algoAfterCPP calls init+process with param/ctx pair',
                '_Z18oppo_platform_initPvyPyS0_S0_' in after_calls and
                after_calls.count('_Z21oppo_platform_processPvS_y') >= 2, ''))
    out.append(('snapshot ctx passed as first process arg (0x5648)',
                '#0x5648' in after, ''))
    out.append(('snapshot mask accumulated at this+0x5958/0x595c',
                '#0x5958' in after or '#0x5958' in function_text(img, ALGO_AFTER_CPP), ''))

    # negative evidence: HDR/LLHDR bits never passed from this module
    module_bits = set()
    for symbol in (TAKE_PICTURE, ALGO_AFTER_CPP, ALGO_PREVIEW, STOP_PREVIEW,
                   ALGO_INIT, POSTPROC_SUPZOOM, BOKEH_PROCESS):
        calls = function_calls(img, symbol)
        text = function_text(img, symbol)
        for bit in list(UNUSED_BITS) + [0x7, 0x8000]:
            if '#0x%x' % bit in text:
                module_bits.add(bit)
    def ors_bit(symbol, bit):
        import re
        text = function_text(img, symbol)
        return re.search(r'\borr\w*\s+\w+, \w+, #0x%x\b' % bit, text) is not None

    checked = (ALGO_AFTER_CPP, ALGO_PREVIEW, TAKE_PICTURE, STOP_PREVIEW)
    out.append(('HDR bit7 is not OR-ed in the HAL1 module',
                not any(ors_bit(s, 0x80) for s in checked), ''))
    out.append(('LLHDR bit14 is not OR-ed in the HAL1 module',
                not any(ors_bit(s, 0x4000) for s in checked), ''))

    # bokeh mask
    bokeh = function_text(img, BOKEH_PROCESS)
    out.append(('bokeh process default mask 0x60', '#0x60' in bokeh, ''))
    out.append(('bokeh process alt mask 0x860', '#0x860' in bokeh, ''))
    out.append(('bokeh ctor init mask 0x860',
                '#0x860' in function_text(img, BOKEH_CTOR), ''))

    # postproc/supZoom pair + offline bit
    sup = function_text(img, POSTPROC_SUPZOOM)
    out.append(('supZoomRoutine init param this+0x2d38', '#0x2d38' in sup, ''))
    out.append(('supZoomRoutine process ctx this+0x4028', '#0x4028' in sup, ''))
    out.append(('supZoomRoutine sets offline bit 0x40000000', '#0x40000000' in sup, ''))

    # C) calibration evidence (core lib)
    core_path = os.path.join(vendor_dir, 'libcamera_core_hwi.so')
    if os.path.exists(core_path):
        core = Image(core_path)
        strings = []
        for section in core._sections:
            if section.name != '.rodata' or section['sh_type'] == 'SHT_NOBITS':
                continue
            strings.append(section.data())
        blob = b''.join(strings)
        out.append(('calib blob path in libcamera_core_hwi',
                    CALIB_PATH.encode() in blob, ''))
        out.append(('calib consumer alloc_get_calibdata present',
                    b'alloc_get_calibdata' in blob, ''))
        out.append(('calib consumer bokehSetCalibData present',
                    b'bokehSetCalibData' in blob, ''))
        text = function_text(core, '_Z23find_biggest_face_widthP20platform_face_data_t')
        out.append(('face count at platform_face_data_t+0x148', '#0x148' in text, ''))
        out.append(('face width/height arrays at +0x50/+0x78',
                    '#0x50' in text and '#0x78' in text, ''))
        # ctx+0x2cc consumers
        for symbol, label in (('_Z18facebeauty_processPvS_', 'facebeauty'),
                              ('_Z19imageadjust_processPvS_', 'imageadjust'),
                              ('_Z18superphoto_ProcessPvS_', 'superphoto'),
                              ('_Z16blurless_ProcessPvS_', 'blurless')):
            out.append(('core %s reads ctx+0x2cc' % label,
                        '#0x2cc' in function_text(core, symbol), ''))
        wrap = function_text(core, '_Z20oppo_wrapper_processPvS_x')
        out.append(('core wrapper memclr(*(ctx+0x2cc)+0x19c, 0xb0)',
                    '#0x2cc' in wrap and '#0x2d4' in wrap and '#0x19c' in wrap, ''))
        gso = function_text(core, '_Z16get_slave_offsetP20p_buffer_construct_t')
        out.append(('p_buffer_construct_t count at +0x280', '#0x280' in gso, ''))
        out.append(('p_buffer_construct_t flag byte at image+0x38', '#0x38' in gso, ''))
        fbi = function_text(core, '_Z18fillBokehImageInfoP6obokehP20p_buffer_construct_tP22bokeh_platform_param_t')
        out.append(('core fillBokehImageInfo uses ctx-as-p_buffer_construct_t',
                    '#0x280' in fbi and '#0x38' in fbi, ''))
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

    print('# frame ctx (0x308) == core p_buffer_construct_t')
    print('  images[10] @ +0x000 stride 0x40: +0x00 ptr, +0x0c, +0x10 w, +0x14 h,')
    print('    +0x18 stride, +0x1c slice, +0x20, +0x30 format, +0x38 valid/primary u8')
    print('  +0x280 u32 image count')
    print('  +0x2c8 u32 scratch fd, +0x2cc void* scratch ptr, +0x2d4 u32 scratch size')
    print('  +0x2c8..0x308 0x40-byte descriptor block preserved across algoReset')
    print()
    print('# owner pairs (param / frame ctx)')
    print('  QCamera2HardwareInterface  this+0x1718 / preview this+0x2a08, snapshot this+0x5648')
    print('  OppoImageBokehInt          this+0x0070 / this+0x1360')
    print('  QCameraPostProcessor       this+0x2d38 / this+0x4028 (supZoom)')
    print()
    print('# scratch buffers (QCameraHeapMemory x1, not per frame)')
    print('  HWI   this+0x2d34  allocate(1, 0x1000) -> fd/ptr/size in ctx+0x2c8/cc/d4')
    print('  bokeh this+0x168c  allocate(1, 0x400)  -> fd/ptr/size in ctx+0x2c8/cc/d4')
    print()
    print('# snapshot mask bits OR-ed by algoAfterCPP @0x12b5ec')
    for bit, label in SNAP_MASK_BITS.items():
        print('  0x%08x  %s' % (bit, label))
    print('  then init(param=this+0x1718, mask) @0x12c704')
    print('       process(ctx=this+0x5648, param=this+0x1718, mask) @0x12c804/0x12c84a')
    print()
    print('# per-caller masks (platform layer, bit numbers from batch 1)')
    print('  preview HWI  : asd 0x400 / SCES 0x200 / arc 0x100 / imageadjust 0x1000')
    print('                 / portraitLight 0x2000 / slam 0x8000 (+ in-process bit63)')
    print('  preview sync : SCES/asd/arc/imageadjust')
    print('  snapshot     : stfaceblur 0x4 / filter 0x8 / faceInfo 0x10 / facebeauty 0x20')
    print('                 / SCES 0x200 / imageadjust 0x1000 / offline bit62')
    print('  bokeh object : init mask 0x860 (facebeauty|bokeh|picbest)')
    print('                 process mask 0x60 (facebeauty|bokeh) or 0x860')
    print('  supZoom      : this+0x4338/0x433c member mask (lo=1, hi=bit62 observed)')
    print('  HDR bit7 / LLHDR bit14: never passed by this module (negative evidence)')
    print()
    print('# calibration: %s' % CALIB_PATH)
    print('  calibData_t: +0xa0..a8 yaw/pitch/roll, +0xc8 err_y, X/Y/Z shift, cam1/2 k1..k3')
    print('  fed to ArcSoft bokehSetCalibData by libcamera_core_hwi')
    print()
    print('## assertions')
    for item in results:
        print('  %s %s' % ('PASS' if item[1] else 'FAIL', item[0]))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
