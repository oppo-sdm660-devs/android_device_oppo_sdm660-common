#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build a stock-dump overlay containing OppoCamera.apk private blobs.

The Lineage extract-files.py only reads regular files from a dump tree, while
the front selfie panorama, Polarr JNI and filter LUTs live inside
OppoCamera.apk. This script hardlink-clones the stock dump and materialises
those entries at their vendor paths, then extract-files.py is run with the
overlay as its source.

Usage:
  ./tools/extract-oppo-camera-apk.py \
      --stock-root /home/cy/Project/R11/stock/R11s \
      --overlay /home/cy/Project/R11/.stock-overlays/R11s-oppocamera
  cd <lineage-root>
  PYTHONPATH=tools/extract-utils python3 \
      device/oppo/sdm660-common/extract-files.py <overlay>
  PYTHONPATH=tools/extract-utils python3 \
      device/oppo/sdm660-common/setup-makefiles.py

The APK is never modified; the stock dump is read-only for us. Re-running with
--force rebuilds the overlay. Generated files are deterministic (copy of APK
entries, no timestamps embedded).
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import zipfile

MARKER = ".oppo-camera-apk-overlay.json"
APK_REL = "system/app/OppoCamera/OppoCamera.apk"

# (APK entry, overlay path). Paths use the vendor layout so proprietary-files.txt
# can reference them like any other dump file. LUT basenames are kept verbatim.
APK_FILES = [
    ("lib/armeabi/libVDSelfiePanorama.so", "vendor/lib/libVDSelfiePanorama.so"),
    ("lib/armeabi/libPolarrRenderJNI.so", "vendor/lib/libPolarrRenderJNI.so"),
    ("assets/filters_johnny_20.json",
     "vendor/etc/camera/oppo_filters/filters_johnny_20.json"),
    ("assets/filters_johnny_32.json",
     "vendor/etc/camera/oppo_filters/filters_johnny_32.json"),
]

LUT_PREFIX = "assets/luts/"
LUT_DEST = "vendor/etc/camera/oppo_filters/luts/"


def fail(msg):
    print(f"ERROR: {msg}", file=sys.stderr)
    sys.exit(1)


def hardlink_clone(src, dst):
    if os.path.exists(dst):
        fail(f"overlay already exists: {dst} (use --force to replace)")
    os.makedirs(os.path.dirname(os.path.abspath(dst)) or ".", exist_ok=True)
    try:
        subprocess.run(["cp", "-al", src, dst], check=True)
        return
    except (OSError, subprocess.CalledProcessError):
        pass
    # Different filesystem or cp lacking hardlink support: copy files and link
    # where possible instead of copying multi-GB again.
    def link_or_copy(s, d, *, follow_symlinks=True):
        try:
            os.link(s, d)
        except OSError:
            shutil.copy2(s, d)
    shutil.copytree(src, dst, copy_function=link_or_copy, symlinks=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stock-root", required=True,
                        help="stock dump root containing vendor/ and system/")
    parser.add_argument("--overlay", required=True,
                        help="directory to create (hardlink clone + APK entries)")
    parser.add_argument("--apk", help=f"APK path (default: <stock-root>/{APK_REL})")
    parser.add_argument("--force", action="store_true",
                        help="replace an existing overlay")
    args = parser.parse_args()

    stock_root = os.path.abspath(args.stock_root)
    apk = os.path.abspath(args.apk or os.path.join(stock_root, APK_REL))
    overlay = os.path.abspath(args.overlay)

    if not os.path.isdir(os.path.join(stock_root, "vendor")):
        fail(f"not a stock dump (no vendor/): {stock_root}")
    if not os.path.isfile(apk):
        fail(f"APK not found: {apk}")

    marker = os.path.join(overlay, MARKER)
    if os.path.exists(overlay):
        if not args.force:
            fail(f"overlay exists: {overlay} (pass --force to rebuild)")
        shutil.rmtree(overlay)
    hardlink_clone(stock_root, overlay)

    entries = {}
    with zipfile.ZipFile(apk) as zf:
        names = set(zf.namelist())
        wanted = list(APK_FILES)
        for name in sorted(names):
            if name.startswith(LUT_PREFIX) and not name.endswith("/"):
                wanted.append((name, LUT_DEST + os.path.basename(name)))
        for entry, rel_dst in wanted:
            if entry not in names:
                fail(f"APK entry missing: {entry}")
            dst = os.path.join(overlay, rel_dst)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with zf.open(entry) as src, open(dst, "wb") as out:
                shutil.copyfileobj(src, out)
            entries[rel_dst] = {"apk_entry": entry, "size": os.path.getsize(dst)}
            print(f"extracted {entry} -> {rel_dst}")

    with open(marker, "w", encoding="utf-8") as f:
        json.dump({"apk": apk, "files": entries}, f, indent=2, sort_keys=True)
        f.write("\n")
    print(f"overlay ready: {overlay} ({len(entries)} APK files)")


if __name__ == "__main__":
    main()
