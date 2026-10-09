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
        'vendor.qti.imsrtpservice@3.0.so',
    ): lib_fixup_vendor_suffix,
}


blob_fixups: blob_fixups_user_type = {
    (
        'system_ext/lib64/lib-imscamera.so',
        'system_ext/lib64/lib-imsvideocodec.so'
    ): blob_fixup()
        .add_needed('libgui_shim.so'),
    'vendor/bin/pm-service': blob_fixup()
        .replace_needed('libutils.so', 'libutils-v33.so'),
    (
        'vendor/lib/libarcsoft_dualcam_bokeh_api.so',
        'vendor/lib/libSonyIMX376RmscLibrary.so',
    ): blob_fixup()
        .replace_needed('libstdc++.so', 'libstdc++_vendor.so'),
    'vendor/lib/libmmcamera_hdr_gb_lib.so': blob_fixup()
        .add_needed('liblog.so')
        .replace_needed('libstdc++.so', 'libstdc++_vendor.so'),
    (
        'vendor/lib/libmmcamera_pdaf.so',
        'vendor/lib/libmmcamera_pdafcamif.so',
        'vendor/lib/libmmcamera_tintless_bg_pca_algo.so',
    ): blob_fixup()
        .add_needed('liblog.so'),
    'vendor/lib/libmmcamera_faceproc.so': blob_fixup()
        .clear_symbol_version('__aeabi_memcpy')
        .clear_symbol_version('__aeabi_memset')
        .clear_symbol_version('__gnu_Unwind_Find_exidx'),
    # Stock SDSPRPC imports resolve to libcdsprpc's unversioned exports.
    'vendor/lib/libthread_blur.so': blob_fixup()
        .clear_symbol_version('remote_handle_open')
        .clear_symbol_version('remote_handle_invoke')
        .clear_symbol_version('remote_handle_close'),
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
    'vendor/lib64/libwvhidl.so': blob_fixup()
        .add_needed('libcrypto_shim.so'),
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
