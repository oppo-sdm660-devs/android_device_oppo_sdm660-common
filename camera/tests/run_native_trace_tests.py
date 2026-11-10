#!/usr/bin/env python3
"""Run the production native trace ring across slot reuse."""
import os
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
HAL3 = HERE.parent / 'QCamera2' / 'HAL3'
source = (HAL3 / 'QCamera3HWI.cpp').read_text()
header = (HAL3 / 'QCamera3HWI.h').read_text()
def function(signature, source=source):
    start = source.index(signature)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]
out = Path(tempfile.mkdtemp(prefix='ocam-native-trace-'))
start = header.index('    static const size_t kNativeTraceSlots =')
end = header.index('\n\n', header.index('    Mutex mNativeTraceLock;', start))
(out / 'state.inc').write_text(header[start:end])
(out / 'functions.inc').write_text('\n\n'.join(function(signature) for signature in [
    'void QCamera3HardwareInterface::noteNativeTrace(',
    'void QCamera3HardwareInterface::noteNativeStage(',
    'void QCamera3HardwareInterface::noteOcclusionResult(',
    'bool QCamera3HardwareInterface::getNativeStageRecords(',
    'bool QCamera3HardwareInterface::getOcclusionResult(',
    'bool QCamera3HardwareInterface::getNativeTrace(']))
(out / 'trace.inc').write_text(function('static void nativeAlgorithmTrace(',
    (HAL3 / 'QCamera3PostProc.cpp').read_text()))
flags = ['-std=gnu++17', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-pthread',
         '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined']
subprocess.run([os.environ.get('CXX', 'g++'), *flags, '-I', str(out), '-I', str(HAL3),
                str(HERE / 'native_trace_test.cpp'), str(HAL3 / 'QCamera3AlgoTrace.cpp'),
                '-o', str(out / 'test')], check=True)
subprocess.run([str(out / 'test')], check=True)
print(f'artifacts: {out}')
