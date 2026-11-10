#!/usr/bin/env python3
"""Run production interface indexed-request admission/consume/cancel under ASan."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent / 'QCamera2/stack/mm-camera-interface'
source = (ROOT / 'src/mm_camera_channel.c').read_text()

def function(signature):
    start = source.index(signature + '\n{')
    return source[start:source.index('\n}\n', start) + 2]

out = Path(tempfile.mkdtemp(prefix='ocam-frame-requests-'))
helpers = source[source.index('static void mm_channel_reset_frame_requests('):
                 source.index('static void mm_channel_process_stream_buf(')]
(out / 'helpers.inc').write_text(helpers)
(out / 'request.inc').write_text(function('int32_t mm_channel_request_super_buf(mm_channel_t *my_obj,\n        mm_camera_req_buf_t *buf)'))
start = source.index('    } else if (MM_CAMERA_CMD_TYPE_REQ_DATA_CB  == cmd_cb->cmd_type) {')
end = source.index('    } else if (MM_CAMERA_CMD_TYPE_START_ZSL', start)
(out / 'receive.inc').write_text(source[source.index('\n', start) + 1:end])
start = source.index('    } else if (MM_CAMERA_CMD_TYPE_STOP_ZSL == cmd_cb->cmd_type) {')
end = source.index('    } else if', start + 10)
(out / 'stop.inc').write_text(source[source.index('\n', start) + 1:end])
start = source.index('        if (info.num_nodes > 0) {')
end = source.index('            if ((m_obj->frame_sync.is_active)', start)
(out / 'consume.inc').write_text(source[source.index('\n', start) + 1:end])
# Consumption must also run for continuous channels, before the burst-only bookkeeping.
assert source.index('mm_channel_consume_frame_request(ch_obj);', start) < source.index('if (MM_CAMERA_SUPER_BUF_NOTIFY_BURST', start)
start = source.index('    } else if (MM_CAMERA_CMD_TYPE_FLUSH_QUEUE  ==')
end = source.index('    } else if (MM_CAMERA_CMD_TYPE_GENERAL ==', start)
(out / 'flush.inc').write_text(source[source.index('\n', start) + 1:end])
capacity = re.search(r'#define MAX_CAPTURE_BATCH_NUM (\d+)',
    (ROOT.parent / 'common/cam_types.h').read_text()).group(1)
flags = ['-std=gnu++17', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-pthread',
         '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined']
subprocess.run([os.environ.get('CXX', 'g++'), *flags, '-DMAX_CAPTURE_BATCH_NUM=' + capacity,
                '-I', str(out), str(HERE / 'frame_request_queue_test.cpp'), '-o', str(out / 'test')], check=True)
subprocess.run([str(out / 'test')], check=True, timeout=30)
print(f'artifacts: {out}')
