#!/usr/bin/env python3
"""Check the actual equal-exposure portrait denoiser, including in-place commit."""
import os
from pathlib import Path
import subprocess
import tempfile

here = Path(__file__).resolve().parent
hal = here.parent / 'QCamera2/HAL3'
with tempfile.TemporaryDirectory(prefix='ocam-portrait-denoise-') as folder:
    binary = Path(folder) / 'test'
    subprocess.run([os.environ.get('CXX', 'g++'), '-std=gnu++17', '-Wall', '-Wextra', '-Werror',
                    '-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined',
                    '-I', str(hal), str(here / 'portrait_denoise_test.cpp'),
                    str(hal / 'QCamera3StillAlgo.cpp'), str(hal / 'QCamera3HdrAlgo.cpp'),
                    '-ldl', '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
