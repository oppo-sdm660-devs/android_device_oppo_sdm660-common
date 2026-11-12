#!/usr/bin/env python3
"""Exercise the ARC_DCVR adapter with a fake DSO; no proprietary image validation."""
import os
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
HAL3 = HERE.parent / 'QCamera2' / 'HAL3'
out = Path(tempfile.mkdtemp(prefix='ocam-stock-preview-bokeh-'))
flags = ['-m32', '-std=gnu++17', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
         '-fno-omit-frame-pointer', '-fsanitize=address,undefined',
         '-fno-sanitize-recover=undefined', '-pthread']
cxx = os.environ.get('CXX', 'g++')
subprocess.run([cxx, *flags, '-shared', '-fPIC', str(HERE / 'fake_arcsoft_preview_bokeh.cpp'),
                '-o', str(out / 'libarcsoft_dualcam_refocus_preview.so')], check=True)
subprocess.run([cxx, *flags, '-I', str(HAL3), str(HERE / 'stock_preview_bokeh_test.cpp'),
                *[str(HAL3 / source) for source in ('QCamera3StockPreviewBokeh.cpp',
                  'QCamera3DualBokehAlgo.cpp', 'QCamera3HdrAlgo.cpp')],
                '-ldl', '-o', str(out / 'test')], check=True)
environment = dict(os.environ)
environment['LD_LIBRARY_PATH'] = str(out)
subprocess.run([str(out / 'test')], env=environment, check=True)
print(f'artifacts: {out}')
