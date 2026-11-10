#!/usr/bin/env python3
"""Exercise the live HAL result join and worker completion boundary under ASan."""
import os
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
HAL3 = HERE.parent / 'QCamera2' / 'HAL3'
source = (HAL3 / 'QCamera3HWI.cpp').read_text()
channel = (HAL3 / 'QCamera3Channel.cpp').read_text()

def function(signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

out = Path(tempfile.mkdtemp(prefix='ocam-deferred-result-'))
(out / 'result.inc').write_text(function('void QCamera3HardwareInterface::orchestrateResult('))
(out / 'record.inc').write_text(function('std::shared_ptr<QCamera3HardwareInterface::AcceptedRequest>\nQCamera3HardwareInterface::recordAsyncResult('))
(out / 'wait.inc').write_text(function('int32_t QCamera3HardwareInterface::waitForAsyncResult('))
jpeg = channel[channel.index('void QCamera3PicChannel::jpegEvtHandle('):]
assert jpeg.index('hw->stopZslSnapshotMode();') < jpeg.index('obj->mChannelCB(NULL,')
assert source.count('deferAsyncFinalMetadata(request->frame_number);') == 2
flags = ['-std=gnu++17', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-pthread',
         '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined']
subprocess.run([os.environ.get('CXX', 'g++'), *flags, '-I', str(out), '-I', str(HAL3),
                str(HERE / 'deferred_result_test.cpp'), '-o', str(out / 'test')], check=True)
subprocess.run([str(out / 'test')], check=True)
print(f'artifacts: {out}')
