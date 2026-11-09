#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Reproduce the stock blob pipeline for sdm660-common:
#   1) hardlink-clone the stock dump and materialise OppoCamera.apk entries
#   2) run Lineage extract-files.py against that overlay
#   3) regenerate the proprietary vendor makefiles
#
# Usage:
#   ./tools/extract-stock-blobs.sh \
#       --stock-root /home/cy/Project/R11/stock/R11s \
#       --lineage-root /home/cy/Project/lineage-22.2 \
#       --overlay /home/cy/Project/R11/.stock-overlays/R11s-oppocamera
#
# The overlay is disposable; the vendor repo is generated, never hand-edited.
set -euo pipefail

STOCK_ROOT="${STOCK_ROOT:-}"
LINEAGE_ROOT="${LINEAGE_ROOT:-}"
OVERLAY="${OVERLAY:-}"
TREE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --stock-root) STOCK_ROOT="$2"; shift 2 ;;
        --lineage-root) LINEAGE_ROOT="$2"; shift 2 ;;
        --overlay) OVERLAY="$2"; shift 2 ;;
        --force) FORCE=1; shift ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

[[ -n "$STOCK_ROOT" ]] || { echo "--stock-root is required" >&2; exit 2; }
[[ -n "$LINEAGE_ROOT" ]] || { echo "--lineage-root is required" >&2; exit 2; }
[[ -n "$OVERLAY" ]] || OVERLAY="$STOCK_ROOT/../.stock-overlays/$(basename "$STOCK_ROOT")-oppocamera"

FORCE_ARGS=()
[[ "${FORCE:-0}" == 1 ]] && FORCE_ARGS=(--force)

echo "== APK overlay: $OVERLAY"
PYTHONPATH="$LINEAGE_ROOT/tools/extract-utils" \
    python3 "$TREE/tools/extract-oppo-camera-apk.py" \
        --stock-root "$STOCK_ROOT" --overlay "$OVERLAY" "${FORCE_ARGS[@]}"

DEVICE_LINK="$LINEAGE_ROOT/device/oppo/sdm660-common"
VENDOR_LINK="$LINEAGE_ROOT/vendor/oppo/sdm660-common"
mkdir -p "$(dirname "$DEVICE_LINK")" "$(dirname "$VENDOR_LINK")"
ln -sfn "$TREE" "$DEVICE_LINK"
ln -sfn "$(dirname "$TREE")/proprietary_vendor_oppo_sdm660-common" "$VENDOR_LINK"

cd "$LINEAGE_ROOT"
echo "== extract proprietary files (Camera section)"
PYTHONPATH="$LINEAGE_ROOT/tools/extract-utils" \
    python3 device/oppo/sdm660-common/extract-files.py -s Camera "$OVERLAY"
echo "== regenerate vendor makefiles"
PYTHONPATH="$LINEAGE_ROOT/tools/extract-utils" \
    python3 device/oppo/sdm660-common/extract-files.py -m "$OVERLAY"
echo "== done; vendor repo under $(dirname "$TREE")/proprietary_vendor_oppo_sdm660-common"
