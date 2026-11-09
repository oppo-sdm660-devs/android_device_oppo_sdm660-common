#!/usr/bin/env python3
"""JPEG content checker: reject uniform/green JPEGs that HAL "success" paths used to deliver.

A real camera JPEG has scene structure (grayscale std well above 1) and is
usually >1 MB for 4608x3456. A zero-filled YUV source encodes to a uniform
(0,135,0)-ish image with grayscale std near 0 and a tiny file.

Usage:
  python3 check_jpeg_content.py IMG [IMG...]
Exit 0 only when every image looks like real scene content.
"""

import sys
from pathlib import Path

try:
    from PIL import Image, ImageStat
except ImportError:
    print("FAIL: Pillow is required (pip install pillow)")
    sys.exit(2)

MIN_BYTES = 1024 * 1024
MIN_STD = 8.0

failures = []
for name in sys.argv[1:]:
    path = Path(name)
    if not path.is_file():
        failures.append(f"{name}: missing")
        continue
    size = path.stat().st_size
    try:
        with Image.open(path) as im:
            gray = im.convert("L")
            stat = ImageStat.Stat(gray)
            std = stat.stddev[0]
            mean = stat.mean[0]
            # Average channel separation catches a uniform green cast too.
            rgb = im.convert("RGB").resize((64, 64))
            px = list(rgb.getdata())
            spread = max(max(p) - min(p) for p in px)
    except Exception as exc:  # noqa: BLE001 - report the actual decode error
        failures.append(f"{name}: decode failed: {exc}")
        continue
    ok = size >= MIN_BYTES and std >= MIN_STD and spread > 8
    print(f"{'OK  ' if ok else 'FAIL'} {name}: {size} B grayMean={mean:.1f} "
          f"grayStd={std:.2f} maxChannelSpread={spread}")
    if not ok:
        failures.append(f"{name}: uniform/zero source (size<{MIN_BYTES} or "
                        f"std<{MIN_STD} or spread<=8)")

if failures:
    print("JPEG CONTENT FAILED:")
    for item in failures:
        print(" -", item)
    sys.exit(1)
print(f"JPEG content passed ({len(sys.argv) - 1} image(s))")
