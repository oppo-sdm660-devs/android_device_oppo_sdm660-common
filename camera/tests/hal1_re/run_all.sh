#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Run every HAL1 closed-source-library analysis and the evidence assertions.
#
# Usage: ./run_all.sh [VENDOR_LIB_DIR]
#   VENDOR_LIB_DIR defaults to the proprietary vendor tree of this device.
#
# Outputs are written to a temporary directory (never into the repository) and
# the path is printed at the end.  Exit code is the verify_evidence.py status,
# so this can gate CI.

set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
VENDOR_LIB_DIR=${1:-/home/cy/Project/R11/proprietary_vendor_oppo_sdm660-common/proprietary/vendor/lib}
PLATFORM="$VENDOR_LIB_DIR/liboppo_platform_hwi.so"
CORE="$VENDOR_LIB_DIR/libcamera_core_hwi.so"

for lib in "$PLATFORM" "$CORE"; do
    if [ ! -f "$lib" ]; then
        echo "missing vendor library: $lib" >&2
        exit 2
    fi
done

OUT=$(mktemp -d /tmp/hal1_re.XXXXXX)
echo "outputs: $OUT"

status=0

cd "$HERE"

./hal1re.py info "$PLATFORM" --imports > "$OUT/platform.info.txt" 2>&1 || status=1
./hal1re.py info "$CORE" --imports     > "$OUT/core.info.txt" 2>&1 || status=1
./hal1re.py syms "$PLATFORM"           > "$OUT/platform.syms.txt" 2>&1 || status=1
./hal1re.py syms "$CORE"               > "$OUT/core.syms.txt" 2>&1 || status=1
./hal1re.py veneers "$PLATFORM"        > "$OUT/platform.veneers.txt" 2>&1 || status=1
./hal1re.py veneers "$CORE"            > "$OUT/core.veneers.txt" 2>&1 || status=1
./dump_registry.py "$PLATFORM" --table platform > "$OUT/platform.registry.txt" 2>&1 || status=1
./dump_registry.py "$CORE" --table core         > "$OUT/core.registry.txt" 2>&1 || status=1
./extract_offsets.py "$PLATFORM"       > "$OUT/platform.offsets.txt" 2>&1 || status=1

# Core-lib call chains used by OCam/docs/hal1-re-hwi.md.
{
    echo '### ocamcore::init'
    ./callgraph.py "$CORE" --callees _ZN10oppocamera8ocamcore4initEv --depth 2
    echo '### ocamcore::processNode'
    ./callgraph.py "$CORE" --callees _ZN10oppocamera8ocamcore11processNodeEPv --depth 2
    echo '### ocamcore::workRoutine'
    ./callgraph.py "$CORE" --callees _ZN10oppocamera8ocamcore11workRoutineEPv --depth 2
    echo '### oppo_wrapper_init'
    ./callgraph.py "$CORE" --callees _Z17oppo_wrapper_initPvx --depth 2
    echo '### oppo_wrapper_process'
    ./callgraph.py "$CORE" --callees _Z20oppo_wrapper_processPvS_x --depth 2
    echo '### oppo_wrapper_offline_cb'
    ./callgraph.py "$CORE" --callees _Z23oppo_wrapper_offline_cbxPKvib --depth 2
} > "$OUT/core.callchains.txt" 2>&1 || status=1

# Second batch: stock HAL1 builder evidence.
STOCK_HAL1=${STOCK_HAL1:-/home/cy/Project/R11/stock/R11/vendor/lib/hw/camera.sdm660.so}
if [ -f "$STOCK_HAL1" ]; then
    ./frame_ctx.py --stock-hal1 "$STOCK_HAL1" > "$OUT/frame_ctx.txt" 2>&1 || status=1
    ./face_info.py --stock-hal1 "$STOCK_HAL1" > "$OUT/face_info.txt" 2>&1 || status=1
    ./mode_mask.py --stock-hal1 "$STOCK_HAL1" > "$OUT/mode_mask.txt" 2>&1 || status=1
    ./snapshot.py --stock-hal1 "$STOCK_HAL1" --vendor-lib-dir "$VENDOR_LIB_DIR" > "$OUT/snapshot.txt" 2>&1 || status=1
    ./face_builder.py --stock-hal1 "$STOCK_HAL1" --vendor-lib-dir "$VENDOR_LIB_DIR" > "$OUT/face_builder.txt" 2>&1 || status=1
    ./beauty_params.py --stock-hal1 "$STOCK_HAL1" --vendor-lib-dir "$VENDOR_LIB_DIR" > "$OUT/beauty_params.txt" 2>&1 || status=1
else
    echo "stock HAL1 not found: $STOCK_HAL1 (second batch skipped)" >&2
fi

# Evidence assertions last; their exit code is the script's result.
if ./verify_evidence.py --vendor-lib-dir "$VENDOR_LIB_DIR" --stock-hal1 "$STOCK_HAL1" | tee "$OUT/verify.txt"; then
    echo "evidence: OK"
else
    echo "evidence: FAILED" >&2
    status=1
fi

echo "outputs kept in $OUT"
exit $status
