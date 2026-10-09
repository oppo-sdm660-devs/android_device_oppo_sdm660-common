# CameraApertureOverlay

Device-side RRO carrying Aperture's configuration keys for oppo sdm660
(R11 / R11s). Read by `OverlaysRepository` in the Aperture app, which resolves the
resource **entry name** inside the packages
`org.lineageos.aperture.auto_generated_rro_product__` (first) and
`org.lineageos.aperture.auto_generated_rro_vendor__`
(`app/src/main/java/org/lineageos/aperture/repositories/OverlaysRepository.kt:34-39`,
`LineageOS/android_packages_apps_Aperture`; the working copy in this workspace is on branch
`lineage-24.0` @ `485c98d` while the upstream-pinned evidence (`../../camera/aperture-hal-trim/WS6_CAMERAX_CONTRACT.md`)
was taken from `lineage-23.2` @ `1120b49b7f24e7ef04213db14a6f2e5e52b4f11c`). That is why this
module's package name is not a free choice. The repository reads the overlay APK's own
resources with `getResourcesForApplication()` (ibid. `:19-31`, `:133-143`), so the RRO
does **not** need to be enabled for the values to take effect - it only has to be
installed, which is why it is added to `PRODUCT_PACKAGES` and not enabled via
`cmd overlay`.

## Wiring (applied)

    # android_device_oppo_sdm660-common/common.mk, "Overlays" block
    PRODUCT_PACKAGES += \
        CameraApertureOverlay

Do **not** also use `DEVICE_PACKAGE_OVERLAYS += overlays/CameraApertureOverlay`: both
mechanisms produce a package with the same name, and Aperture resolves exactly one of
them by name.

## What is overridden

| Key | Active value | Reason |
|---|---|---|
| `config_enableHighResolution` | `true` | Already the app default (Aperture `res/values/config.xml:86`); pinned explicitly. |
| `config_additionalVideoConfigurations` | removals only (`uhd -60\|-120`, `sd\|hd\|fhd -120`) for camera ids 0/1/2/3/4 | Keeps Aperture from offering grades outside the declared encoder envelope: AVC/HEVC declare `max 3840x2160`, `blocks-per-second max 972000` (= 4K30 exactly) in `configs/media/media_codecs_vendor.xml:134-161`, and >=120 fps is admitted by the HAL only in a constrained-high-speed session, which Aperture never opens. Both UHD removals are expected no-ops: no R11/R11s sensor mode is 3840x2160 (`WS6_STOCK_VIDEO_EVIDENCE.md` §3.2), so CameraX has no UHD quality to attach a rate to. |
| `config_enableAuxCameras` | `true` | Without it Aperture drops every id outside its always-kept `{0,1}` (`CameraRepository.kt:119,128,151`) - including the logical camera. |
| `config_ignoreLogicalAuxCameras` | `false` | Upstream default `true` drops every camera with `physicalCameraInfos.size > 1` (`CameraRepository.kt:125-126`); with the multi-camera fix the HAL publishes exactly such a device (public id 4 over physical ids 0 and 2). |
| `config_ignoredAuxCameraIds` | empty | 0/1 main, 2 standalone rear aux, 3 paired device, 4 logical. Add `3` only after a device read-back shows it duplicates the pair in the lens selector. |
| `config_logicalZoomRatios` | empty | Snap points must be the unit's real optical ratios, and the stock ColorOS camera app ships a continuous zoom seek bar rather than per-lens buttons, so no ratio can be derived from the dumps. Fill in after reading back `ZOOM_RATIO_RANGE` and `LENS_INFO_AVAILABLE_FOCAL_LENGTHS`. |

## Where the added video grades come from

Adding a frame rate in this overlay is *not* the lever for "more video grades" on this
platform, and the earlier revision of this README was wrong to present it as one:

* Aperture can only turn a **fixed** target-FPS range into a chip
  (`models/FrameRate.kt:18-22`), while `aeAvailableTargetFpsRanges` is filled from the
  device's decoded vendor table (`QCamera2/HAL3/QCamera3HWI.cpp`, decoded in
  `QCamera2/stack/mm-camera-interface/src/mm_camera_oppo_capability.c`).
* The frames are now derived in the HAL: `QCamera2/HAL3/QCamera3VideoGrades.h`
  publishes the vendor pairs, the fixed endpoints of each pair, and `[60,60]` when the
  decoded `CAM_HFR_MODE_60FPS` tier reaches 1080p. `QCamera3HWI.cpp` accepts an exact
  published pair or a sub-range of one, and still refuses anything outside them.
  On the R11/R11s dumps this rule is expected to add little: the stock ColorOS build
  has no 60 fps video grade at all, the main sensor's static mode table tops out at
  30 fps, and the only 60/90 fps sensor modes found (aux `imx376k`) have no HFR tuning
  (`camera/aperture-hal-trim/WS6_STOCK_VIDEO_EVIDENCE.md` §3.2/§4). It stays in place
  because it is driven by the device's own decoded tables, never by an assumption.
* A static overlay entry, by contrast, cannot know whether *this* unit's sensor table
  carries the rate. An invented chip means a request the HAL refuses, i.e. a broken
  recording session rather than a lower frame rate. So the overlay stays removal-only.

Quality (SD/HD/FHD/UHD) is likewise not overlay-controllable, but the lever is a
different file than this README used to claim: CameraX's default
`VIDEO_CAPABILITIES_SOURCE_CAMCORDER_PROFILE` derives the qualities from
`CamcorderProfile`/`EncoderProfiles`, i.e. from `configs/media/media_profiles_vendor.xml`.
A UHD chip therefore appears when that XML has a `2160p` profile for the camera id
*and* the HAL can stream the size - and a `qhd` profile anywhere makes CameraX report
`Quality.QHD`, which Aperture throws on ("Unknown video quality"). The tree disables
qhd/2k/4kdci/2160p for ids 2/3 accordingly; ids 0/1 never had them (OPPO's
"Remove for not support 4K recording." comment).

## Verify after flashing

```sh
adb shell cmd overlay list | grep -i aperture          # package present
adb shell dumpsys package org.lineageos.aperture.auto_generated_rro_product__ | head
adb shell dumpsys media.camera | grep -A4 aeAvailableTargetFpsRanges
# expected: the vendor pairs plus their fixed endpoints, plus [60,60] only when a
# 1080p+ 60 fps HFR tier exists (compare with 'availableHighSpeedVideoConfigurations',
# which must stay absent because persist.vendor.camera.hal3hfr.enable=0)
adb shell dumpsys media.camera | grep -A4 availableStreamConfigurations | grep -i 3840
# then in Aperture: VIDEO mode must offer no 120 chip (nor UHD 60); lens selector:
# 0 (main), 2 (aux) and - only if the paired publication succeeded - 3 (paired) /
# 4 (logical); zoom drag past 1.29 must switch wide -> tele without
# ERROR_CAMERA_DEVICE. See device-tree-nonsource-20261017/camera/docs/MULTICAM_STATUS.md.
```
