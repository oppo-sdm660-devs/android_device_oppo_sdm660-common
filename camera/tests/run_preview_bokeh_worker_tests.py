#!/usr/bin/env python3
"""Verify owned dual-source pairing with a fake ARC DSO, not stock image quality."""
import os
from pathlib import Path
import subprocess
import tempfile

here = Path(__file__).resolve().parent
hal = here.parent / 'QCamera2' / 'HAL3'
out = Path(tempfile.mkdtemp(prefix='ocam-preview-bokeh-worker-'))
flags = ['-m32', '-std=gnu++17', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
         '-fno-omit-frame-pointer', '-fsanitize=address,undefined',
         '-fno-sanitize-recover=undefined', '-pthread']
cxx = os.environ.get('CXX', 'g++')
subprocess.run([cxx, *flags, '-shared', '-fPIC', str(here / 'fake_arcsoft_preview_bokeh.cpp'),
                '-o', str(out / 'libarcsoft_dualcam_refocus_preview.so')], check=True)
subprocess.run([cxx, *flags, '-I', str(hal), str(here / 'preview_bokeh_worker_test.cpp'),
                *[str(hal / source) for source in ('QCamera3PreviewBokehWorker.cpp',
                  'QCamera3StockPreviewBokeh.cpp', 'QCamera3DualBokehAlgo.cpp', 'QCamera3HdrAlgo.cpp')],
                '-ldl', '-o', str(out / 'test')], check=True)
environment = dict(os.environ, LD_LIBRARY_PATH=str(out))
subprocess.run([str(out / 'test')], env=environment, check=True)
print(f'artifacts: {out}')
