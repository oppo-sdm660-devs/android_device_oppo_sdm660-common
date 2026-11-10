#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Reference CPU sampler for the stock Polarr YUV-domain cubes."""
def sample(cube, y, u, v):
    yy, uu, vv = [min(1.0, max(0.0, c)) * 15.0 for c in (y, u, v)]
    low = [int(c) for c in (yy, uu, vv)]
    frac = [c - l for c, l in zip((yy, uu, vv), low)]
    out = [0.0, 0.0, 0.0]
    for by in range(2):
        for bu in range(2):
            for bv in range(2):
                flags = (by, bu, bv)
                p = [min(15, low[a] + flags[a]) for a in range(3)]
                weight = 1.0
                for a in range(3):
                    weight *= frac[a] if flags[a] else 1 - frac[a]
                offset = (p[0] + 16 * p[1] + 256 * p[2]) * 4
                for channel in range(3):
                    out[channel] += cube[offset + channel] * weight / 255
    return tuple(out)


def byte(value):
    return min(255, max(0, int(value * 255 + 0.5)))


def apply_nv21(cube, source, width, height):
    """Reproduce stock shader Y and VU sampling (tightly packed even NV21)."""
    if width < 2 or height < 2 or width % 2 or height % 2 or len(source) != width * height * 3 // 2:
        raise ValueError('expected tightly packed even NV21')
    if len(cube) != 16384:
        raise ValueError('expected 16-cube RGBA8')
    result = bytearray(len(source))
    offset = width * height
    for row in range(height):
        for col in range(width):
            uv = offset + row // 2 * width + (col & ~1)
            y, u, v = sample(cube, source[row * width + col] / 255,
                              source[uv + 1] / 255, source[uv] / 255)
            result[row * width + col] = byte(y)
            if not row % 2 and not col % 2:
                # getY's half-texel row combines adjacent rows, then dot(y.xy,
                # vec2(.5)) combines adjacent columns: one 2x2 luma mean.
                yy = sum(source[(row + dy) * width + col + dx]
                         for dy in range(2) for dx in range(2)) / 1020
                _, u, v = sample(cube, yy, source[uv + 1] / 255, source[uv] / 255)
                result[uv] = byte(v)
                result[uv + 1] = byte(u)
    return bytes(result)
