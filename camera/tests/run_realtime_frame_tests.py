#!/usr/bin/env python3
"""Run extracted production realtime image/metadata join and stage commits."""
import os
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
source = (HERE.parent / 'QCamera2/HAL3/QCamera3HWI.cpp').read_text()


def function(signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


out = Path(tempfile.mkdtemp(prefix='ocam-realtime-frame-'))
for name, signature in (
        ('requested', 'static bool oppoRealtimeRequested('),
        ('session', 'static bool oppoRealtimeSession('),
        ('cache', 'void QCamera3HardwareInterface::cacheRealtimeSource('),
        ('failure', 'void QCamera3HardwareInterface::noteRealtimeFailure('),
        ('process', 'bool QCamera3HardwareInterface::processRealtimeFrame(')):
    (out / f'{name}.inc').write_text(function(signature))
flags = ['-std=gnu++17', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-pthread',
         '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined']
subprocess.run([os.environ.get('CXX', 'g++'), *flags, '-I', str(out),
                str(HERE / 'realtime_frame_test.cpp'), '-o', str(out / 'test')], check=True)
subprocess.run([str(out / 'test')], check=True)
print(f'artifacts: {out}')
