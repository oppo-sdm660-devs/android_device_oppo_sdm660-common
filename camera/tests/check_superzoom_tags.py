#!/usr/bin/env python3
"""Check append-only SuperZoom tag IDs, types and global registration."""
from pathlib import Path
import re

HAL3 = Path(__file__).resolve().parents[1] / "QCamera2/HAL3"
header = (HAL3 / "QCamera3VendorTags.h").read_text()
source = (HAL3 / "QCamera3VendorTags.cpp").read_text()
body = header.split("QCAMERA3_NATIVE_ALGORITHM_MODE = QCAMERA3_NATIVE_ALGORITHMS_START,")[1]
body = body.split("QCAMERA3_NATIVE_ALGORITHMS_END")[0]
enums = ["QCAMERA3_NATIVE_ALGORITHM_MODE"] + re.findall(r"^\s*(QCAMERA3_NATIVE_ALGORITHM_\w+),", body, re.M)
info = re.search(r"qcamera3_native_algorithms\[.*?\] = \{(.*?)\n\};", source, re.S).group(1)
entries = re.findall(r'\{ "(\w+)", (TYPE_\w+) \}', info)
assert len(enums) == len(entries) == 68
expected = [("SUPER_ZOOM_SUPPORTED", "superZoomSupported", "TYPE_BYTE"),
            ("SUPER_ZOOM", "superZoom", "TYPE_INT32"),
            ("APPLIED_SUPER_ZOOM", "appliedSuperZoom", "TYPE_INT32")]
assert enums[64] == "QCAMERA3_NATIVE_ALGORITHM_PANORAMA_FAILURE_REASON"
for offset, (symbol, name, kind) in enumerate(expected, 65):
    tag = "QCAMERA3_NATIVE_ALGORITHM_" + symbol
    assert enums[offset] == tag and entries[offset] == (name, kind)
    assert source.count("(uint32_t)" + tag + ",") == 1
print("SuperZoom append-only tag contract: PASS")
