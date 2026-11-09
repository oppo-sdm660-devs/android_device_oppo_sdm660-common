# Stock blob extraction tools

## Why an overlay

Lineage `extract-files.py` only reads regular files from a dump tree. The OppoCamera APK
private libraries/LUTs live inside the APK, so `extract-oppo-camera-apk.py` hardlink-clones
the stock dump and materialises those entries at their vendor paths. `extract-files.py` is
then pointed at the overlay.

## Run

```sh
cd <device-tree>
./tools/extract-stock-blobs.sh \
  --stock-root /path/to/stock/dump \
  --lineage-root /path/to/lineage \
  --overlay /path/to/disposable/overlay \
  --force
```

The script creates temporary `device/oppo/<device>` and `vendor/oppo/<device>` symlinks in
the Lineage tree for the generator, prunes nothing from the vendor repo, extracts only the
`Camera` section, then regenerates `Android.bp`/`sdm660-common-vendor.mk`. Do not hand-edit
those generated files.

## Extending the APK mapping

Add entries to `APK_FILES` or the LUT prefix handling in
`extract-oppo-camera-apk.py`; keep target paths under `vendor/...` and add the matching
`src:dst|sha1` line to `proprietary-files.txt`. Re-run the wrapper and verify the generated
module/install rule and checksum.

## Known limits

- APK native libs are 32-bit; app namespace and vendor namespace have different visibility
  (documented in `docs/hal-changes.md` §29).
- Stock MMS libraries still need their original `libui` ABI (see §29.3); adding an
  unapproved stock-libui shim is intentionally not done here.
