#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Cross-check ELF extraction and CPU sampling against stock GLSL on Mesa EGL."""
import argparse
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

CAMERA = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(CAMERA / 'tools'))
from extract_stock_filter_luts import extract, make_catalog
from stock_filter_lut import apply_nv21, byte, sample


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--lib', required=True, type=Path)
    parser.add_argument('--device-dir', type=Path,
                        help='Outputs of filter_polarr_smoke.cpp: type-0.nv21..type-20.nv21 at 64x48')
    parser.add_argument('--skip-gl', action='store_true')
    args = parser.parse_args()
    data = args.lib.read_bytes()
    cubes, evidence = extract(data)
    catalog = make_catalog(data, cubes, evidence)
    assert [e['type'] for e in catalog['frontMenu']] == [0, 1, 2, 3, 5, 4, 10, 6, 7, 8]
    assert [e['type'] for e in catalog['backMenu']] == [0, 1, 2, 3, 5, 4, 12, 13, 15, 14]
    assert catalog['sampler'].startswith('GL_LINEAR')
    for filter_type, cube in enumerate(cubes):
        assert len(cube) == 16384
        assert set(cube[3::4]) == {255}
        assert hashlib.sha256(cube).hexdigest() == catalog['filterTypes'][filter_type]['sha256']
        for y in (0, 15):
            for u in (0, 15):
                for v in (0, 15):
                    pos = (y + 16 * u + 256 * v) * 4
                    assert tuple(byte(c) for c in sample(cube, y / 15, u / 15, v / 15)) == tuple(cube[pos:pos + 3])
    damaged = bytearray(data)
    # Alter a proven initFilter instruction; extraction must reject another ABI.
    from extract_stock_filter_luts import Elf32
    elf = Elf32(data)
    start = evidence['symbolAddress']
    for _, offset, va, _, filesz, _, _, _ in elf.loads:
        if va <= start < va + filesz:
            damaged[offset + start - va] ^= 1
            break
    try:
        extract(damaged)
        raise AssertionError('unrecognized instructions were accepted')
    except ValueError:
        pass
    identity = b''.join(bytes((y * 17, u * 17, v * 17, 255))
                        for v in range(16) for u in range(16) for y in range(16))
    q = sample(identity, 0.12, 0.123, 0.34)
    assert max(abs(a - b) for a, b in zip(q, (0.12, 0.123, 0.34))) < 1e-8
    if args.device_dir:
        width, height = 64, 48
        source = bytes((x * 7 + y * 13) & 255 for y in range(height) for x in range(width))
        source += b''.join(bytes(((x * 3 + y * 17) & 255, (x * 11 + y * 5) & 255))
                           for y in range(height // 2) for x in range(0, width, 2))
        for filter_type, cube in enumerate(cubes):
            actual = (args.device_dir / f'type-{filter_type}.nv21').read_bytes()
            assert len(actual) == len(source)
            expected = apply_nv21(cube, source, width, height)
            max_error = max(abs(a - b) for a, b in zip(actual, expected))
            changed = sum(a != b for a, b in zip(actual, source))
            assert max_error <= 1 and changed > 0, (filter_type, max_error, changed)
            print(f'type={filter_type}: device stock Polarr vs CPU NV21 {len(actual)} bytes, maxByteError={max_error}, changedBytes={changed}')
    if args.skip_gl:
        print('PASS: extraction, format, menu, invalid ABI and supplied device pixels; GL skipped explicitly')
        return
    stock_function = data[data.index(b'vec3 lookup(in float Y, in float V, in float U)'):]
    stock_function = stock_function[:stock_function.index(b'}') + 1].decode()
    fragment = 'precision highp float;\nuniform sampler2D lut_texture;\nuniform float lut_size;\n' + stock_function + '''
void main() {
  vec3 col = vec3((gl_FragCoord.x-.5)/255., (gl_FragCoord.y-.5)/255.,
                  mod(gl_FragCoord.x+3.*gl_FragCoord.y-2.,256.)/255.);
  gl_FragColor=vec4(lookup(col.x,col.z,col.y).xzy,1.);
}
'''
    with tempfile.TemporaryDirectory(prefix='ocam-filter-gl-') as temporary:
        root = Path(temporary)
        binary, shader = root / 'gl', root / 'stock.frag'
        shader.write_text(fragment)
        subprocess.run(['g++', '-O2', '-Wall', '-Wextra', '-Werror',
                        str(CAMERA / 'tests/stock_filter_lut_gl.cpp'), '-lEGL', '-lGLESv2',
                        '-o', str(binary)], check=True)
        for filter_type, cube in enumerate(cubes):
            lut, output = root / 'lut', root / 'output'
            lut.write_bytes(cube)
            subprocess.run([str(binary), str(lut), str(output), str(shader)],
                           env={**os.environ, 'EGL_PLATFORM': 'surfaceless'}, check=True)
            gpu = output.read_bytes()
            max_error = 0
            for yy in range(256):
                for xx in range(256):
                    offset = (yy * 256 + xx) * 4
                    expected = tuple(byte(c) for c in sample(cube, xx / 255, yy / 255,
                                                            (xx + 3 * yy) % 256 / 255))
                    max_error = max(max_error, *(abs(a - b) for a, b in zip(expected, gpu[offset:offset + 3])))
                    assert gpu[offset + 3] == 255
            assert max_error <= 1, (filter_type, max_error)
            print(f'type={filter_type}: stock lookup GLSL vs CPU 65536 samples, maxByteError={max_error}')
    print('PASS: 21 embedded cubes, menu/format/error checks, 1376256 stock GLSL samples; no camera acceptance')


if __name__ == '__main__':
    main()
