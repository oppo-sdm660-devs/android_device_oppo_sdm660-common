#!/usr/bin/env python3
"""Extract production dual burst orchestration; fake only Camera2/driver boundaries."""
import os
from pathlib import Path
import subprocess
import tempfile

here = Path(__file__).resolve().parent
hal = here.parent / 'QCamera2' / 'HAL3'
source = (hal / 'QCamera3HWI.cpp').read_text()
signature = 'int32_t QCamera3HardwareInterface::orchestrateDualCapture('
start = source.index(signature)
opening = source.index('{', start)
depth, end = 1, opening + 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
out = Path(tempfile.mkdtemp(prefix='ocam-dual-burst-orchestration-'))
(out / 'dual_burst.inc').write_text(source[start:end])
prepare = source.index('if (mDualBurstPending)', source.index('// Snapshot streams now have their actual sensor dimensions.'))
opening = source.index('{', prepare)
depth, end = 1, opening + 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
(out / 'dual_prepare.inc').write_text(source[prepare:end])
assert source.index('getSensorOutputSize(sensor_dim)') < prepare < source.index('// Acquire all request buffers first', prepare)
flags = ['-m32', '-std=gnu++17', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-pthread',
         '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined']
subprocess.run([os.environ.get('CXX', 'g++'), *flags, '-I', str(out),
                str(here / 'dual_burst_orchestration_test.cpp'), '-o', str(out / 'test')], check=True)
subprocess.run([str(out / 'test')], check=True)
print(f'artifacts: {out}')
