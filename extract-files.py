#!/usr/bin/env -S PYTHONPATH=../../../tools/extract-utils python3
#
# SPDX-FileCopyrightText: 2024 The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#

from extract_utils.file import File
from extract_utils.fixups_blob import (
    BlobFixupCtx,
    blob_fixup,
    blob_fixups_user_type,
)
from extract_utils.fixups_lib import (
    lib_fixup_remove,
    lib_fixups,
    lib_fixups_user_type,
)
from extract_utils.main import (
    ExtractUtils,
    ExtractUtilsModule,
)

namespace_imports = [
    'device/oppo/sdm660-common',
    'hardware/qcom-caf/sdm660',
    'hardware/qcom-caf/wlan',
    'vendor/qcom/opensource/dataservices',
    'vendor/qcom/opensource/display',
]


def lib_fixup_vendor_suffix(lib: str, partition: str, *args, **kwargs):
    return f'{lib}_{partition}' if partition == 'vendor' else None


lib_fixups: lib_fixups_user_type = {
    **lib_fixups,
    (
        'com.qualcomm.qti.dpm.api@1.0',
        'vendor.qti.hardware.fm@1.0',
    ): lib_fixup_vendor_suffix,
}


blob_fixups: blob_fixups_user_type = {
    'vendor/bin/pm-service': blob_fixup()
        .replace_needed('libutils.so', 'libutils-v33.so'),
    'vendor/lib/hw/camera.sdm660.so': blob_fixup()
        .sig_replace(
            "4F F4 51 73 C1 68 D0 E9 0D 20 CD E9 03 20 4B 48",
            "4F F4 51 73 C1 68 D0 E9 12 20 CD E9 03 20 4B 48"
        )
        .sig_replace(
            "C0 68 CB F8 08 00 31 68 08 9A 00 26 49 6B CB E9 04 21",
            "C0 68 CB F8 08 00 31 68 08 9A 00 26 89 6C CB E9 04 21"
        )
        .sig_replace(
            "D4 F8 7C 39 01 22 C8 68 49 6B E0 47 C5 F8 80 09 3E E0",
            "D4 F8 7C 39 01 22 C8 68 89 6C E0 47 C5 F8 80 09 3E E0"
        )
        .sig_replace(
            "DB F8 00 00 CA 68 D1 E9 0D 31 05 91 CD E9 03 03 20 48",
            "DB F8 00 00 CA 68 D1 E9 12 31 05 91 CD E9 03 03 20 48"
        )
        .sig_replace(
            "30 68 3D 46 C0 68 CA F8 08 00 30 68 06 99 40 6B CA E9 04 10",
            "30 68 3D 46 C0 68 CA F8 08 00 30 68 06 99 80 6C CA E9 04 10"
        )
        .sig_replace(
            "D4 F8 7C 39 01 22 C8 68 49 6B E0 47 C5 F8 80 09 5A E0",
            "D4 F8 7C 39 01 22 C8 68 89 6C E0 47 C5 F8 80 09 5A E0"
        )
        .sig_replace(
            "DB F8 00 00 CA 68 D1 E9 0D 31 05 91 CD E9 03 03 21 48",
            "DB F8 00 00 CA 68 D1 E9 12 31 05 91 CD E9 03 03 21 48"
        )
        .sig_replace(
            "38 68 C0 68 CA F8 08 00 38 68 06 99 40 6B CA E9 04 10 B2 46",
            "38 68 C0 68 CA F8 08 00 38 68 06 99 80 6C CA E9 04 10 B2 46"
        )
        .sig_replace(
            "CD F8 2C 80 C8 68 49 6B B8 47 DD F8 2C 80 C5 F8 80 09",
            "CD F8 2C 80 C8 68 89 6C B8 47 DD F8 2C 80 C5 F8 80 09"
        )
        .sig_replace(
            "DF F8 B0 76 E0 68 CA 68 7F 44 D1 E9 0D 31 CD E9 00 78",
            "DF F8 B0 76 E0 68 CA 68 7F 44 D1 E9 12 31 CD E9 00 78"
        )
        .sig_replace(
            "D6 F8 58 08 10 99 40 6B C4 E9 04 10 01 20",
            "D6 F8 58 08 10 99 80 6C C4 E9 04 10 01 20"
        ),
    (
        'vendor/lib/libarcsoft_dualcam_bokeh_api.so',
        'vendor/lib/libarcsoft_dualcam_refocus_left.so',
        'vendor/lib/libarcsoft_dualcam_refocus_preview.so',
        'vendor/lib/libarcsoft_dualcam_refocus_right.so',
        'vendor/lib/libarcsoft_smart_denoise.so',
        'vendor/lib/libfilter.so',
        'vendor/lib/libmmcamera_hdr_gb_lib.so'
    ): blob_fixup()
        .replace_needed('libstdc++.so', 'libstdc++_vendor.so'),
    'vendor/lib/libmmcamera_interface.so': blob_fixup()
        .binary_regex_replace(
            b'/system/etc/camera',
            b'/vendor/etc/camera'
        ),
    'vendor/lib/libmmcamera2_sensor_modules.so': blob_fixup()
        .sig_replace(
            '32 D8 DF E8 07 F0 36 03 03 3B 3E 00 00 26 9D 20 39 E0',
            '32 D8 DF E8 07 F0 36 31 31 3B 3E 00 00 26 9D 20 39 E0'
        ),
    'vendor/lib64/libcdsprpc.so': blob_fixup()
        .sig_replace(
            'FD 7B 02 A9 FD 83 00 91 13 04 00 12 7F 0E 00 71 C1 01 00 54 A9 00 00 B0 68 02 1F 52 29 81 00 91 20 59 68 F8',
            'FD 7B 02 A9 FD 83 00 91 13 04 00 12 7F 0E 00 71 0E 00 00 14 A9 00 00 B0 68 02 1F 52 29 81 00 91 20 59 68 F8'
        ),
    'vendor/lib64/libqmiservices.so': blob_fixup()
        .sig_replace(
            '0B 00 00 00 23 00 0D 00 00 00 24 00 0F 00 00 00 25 00 11 00 00 00 26 00 15 00 00 00 27 00 17 00 16 00 28 00 19 00',
            '0B 00 00 00 23 00 0D 00 00 00 24 00 0F 00 00 00 25 00 55 00 04 00 26 00 15 00 00 00 27 00 17 00 16 00 28 00 19 00'
        ),
    'vendor/lib64/libril-qc-hal-qmi.so': blob_fixup()
        .sig_replace(
            '06 A6 8E 52 E3 C7 41 B9 E4 BF 40 F9 E5 77 41 B9 42 7F 35 94 E0 EB 01 B9 E0 EB 41 B9',
            '06 A6 8E 52 E3 C7 41 B9 E4 BF 40 F9 E5 77 41 B9 DC F3 EE 97 E0 EB 01 B9 E0 EB 41 B9'
        )
        .sig_replace(
            'E0 02 80 52 CE 80 46 94 C0 04 00 34 D8 8C 46 94 E0 3B 00 B9 E8 3B 40 B9 1F 01 00 71 E8 B7 9F 1A 08 04 00 37',
            'E0 02 80 52 CE 80 46 94 C0 04 00 34 D8 8C 46 94 E0 3B 00 B9 E8 3B 40 B9 1F 01 00 71 E8 B7 9F 1A 20 00 00 14'
        )
        .sig_replace(
            '20 BC FF B0 00 E8 17 91 84 80 46 94 E0 17 00 F9 86 80 46 94 E0 13 00 F9 88 80 46 94 E1 03 00 2A 26 7C 40 93 E6 0F 00 F9 73 FF FF 97 E8 3B 40 B9 A9 00 80 52 E0 0B 00 F9 E0 03 09 2A 21 BE FF 90 21 90 2D 91 A2 BF FF F0 42 48 27 91',
            'FF 03 01 D1 FD 7B 03 A9 FD C3 00 91 E0 13 00 A9 E5 1B 01 A9 39 40 FC 97 08 04 00 11 E8 83 00 39 E0 03 40 F9 A1 04 80 52 E2 83 00 91 23 00 80 52 E4 07 40 F9 E5 13 40 B9 E6 1B 40 B9 CD FF FF 97 FD 7B 43 A9 FF 03 01 91 C0 03 5F D6'
        ),
}  # fmt: skip

module = ExtractUtilsModule(
    'sdm660-common',
    'oppo',
    blob_fixups=blob_fixups,
    lib_fixups=lib_fixups,
    namespace_imports=namespace_imports,
)

if __name__ == '__main__':
    utils = ExtractUtils.device(module)
    utils.run()
