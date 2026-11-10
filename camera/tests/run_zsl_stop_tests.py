#!/usr/bin/env python3
"""Exercise the real interface stop enqueue/ack with a blocked daemon call."""
import os
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent / 'QCamera2' / 'stack' / 'mm-camera-interface'
source = (ROOT / 'src/mm_camera_channel.c').read_text()
start = source.index('int32_t mm_channel_stop_zsl_snapshot(mm_channel_t *my_obj)\n{')
end = source.index('\n}\n', start) + 2
branch = source.index('    } else if (MM_CAMERA_CMD_TYPE_STOP_ZSL == cmd_cb->cmd_type) {')
branch_start = source.index('\n', branch) + 1
branch_end = source.index('    } else if', branch_start)
ack = source.index('    if (MM_CAMERA_CMD_TYPE_STOP_ZSL == cmd_cb->cmd_type)\n')
ack_end = source.index('\n}', ack)
assert 'stop_zsl_snapshot_rc' in (ROOT / 'inc/mm_camera.h').read_text()
assert source.count('mm_channel_stop_zsl_snapshot(my_obj);') == 1
out = Path(tempfile.mkdtemp(prefix='ocam-zsl-stop-'))
reset = source.index('static void mm_channel_reset_frame_requests(')
(out / 'reset.inc').write_text(source[reset:source.index('\n}\n', reset) + 2])
(out / 'stop.inc').write_text(source[start:end])
(out / 'daemon.inc').write_text(source[branch_start:branch_end])
(out / 'ack.inc').write_text(source[ack:ack_end])
flags = ['-std=gnu++17', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-pthread',
         '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined']
subprocess.run([os.environ.get('CXX', 'g++'), *flags, '-I', str(out),
                str(HERE / 'zsl_stop_test.cpp'), '-o', str(out / 'test')], check=True)
subprocess.run([str(out / 'test')], check=True)
print(f'artifacts: {out}')
