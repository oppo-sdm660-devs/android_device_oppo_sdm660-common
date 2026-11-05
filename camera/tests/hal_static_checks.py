#!/usr/bin/env python3
"""Static consistency checks for the T06 HAL port round (grep level, not a build).

Verifies that the coupled edits stay coupled: standard-entry predicates, scene
mode announcement, template defaults, 4K/vstab property wiring, front beauty
protocol/consumer and the Android.bp source list.
"""

import re
import sys
from pathlib import Path

CAMERA = Path(__file__).resolve().parents[1]
HAL3 = CAMERA / "QCamera2" / "HAL3"
VENDOR_PROP = CAMERA.parent / "vendor.prop"

failures = []
checks = 0


def read(path):
    try:
        return path.read_text(errors="replace")
    except OSError:
        failures.append(f"missing file: {path}")
        return ""


def need(path, text, note):
    global checks
    checks += 1
    if text not in read(path):
        failures.append(f"{path.name}: missing {note}: {text[:70]!r}")


def absent(path, text, note):
    global checks
    checks += 1
    if text in read(path):
        failures.append(f"{path.name}: unexpected {note}: {text[:70]!r}")


hwi = HAL3 / "QCamera3HWI.cpp"
post = HAL3 / "QCamera3PostProc.cpp"
channel = HAL3 / "QCamera3Channel.cpp"
header = HAL3 / "QCamera3HALHeader.h"
hwih = HAL3 / "QCamera3HWI.h"
bp = CAMERA / "Android.bp"

# A. 4K property wiring and the final video-stabilization removal.
for prop in ("persist.vendor.camera.oppo.video.allow_4k=1",
             "persist.vendor.camera.oppo.video.picture_overlap=1",
             "persist.vendor.camera.oppo.eis.extension=0",
             "persist.vendor.camera.oppo.vstab.mms=0"):
    need(VENDOR_PROP, prop, "device property")
absent(VENDOR_PROP, "vstab.publish", "removed publish property")
need(hwi, "persist.vendor.camera.oppo.video.allow_4k", "4K gate")
need(hwi, "persist.vendor.camera.oppo.vstab.mms", "MMS gate")
need(hwi, "persist.vendor.camera.oppo.eis.extension", "EIS2 extension gate")
need(hwi, "const bool mmsCandidate = oppoMmsVstabEnabled()", "MMS candidate gate")

# Video stabilization: standard path is OFF only (no EIS2/MMS by default).
absent(hwi, "oppoVstabPublishEnabled", "removed publish helper")
absent(hwi, "availableVstabModes.add(ANDROID_CONTROL_VIDEO_STABILIZATION_MODE_ON)",
       "ON announcement")
need(hwi, "availableVstabModes.add(ANDROID_CONTROL_VIDEO_STABILIZATION_MODE_OFF)",
     "OFF-only announcement")
need(hwi, "ANDROID_CONTROL_VIDEO_STABILIZATION_MODE_PREVIEW_STABILIZATION",
     "accept values without enabling")
absent(hwi, "!oppoEisSupported(capability)", "old EIS availability rejection")
need(hwi, "uint8_t fwk_videoStab = ANDROID_CONTROL_VIDEO_STABILIZATION_MODE_OFF;",
     "standard result forced OFF")
need(hwi, "if (oppoVideoStabExtensionEnabled())", "result extension gate")
need(hwi, "m_bEisEnable = m_bEisSupported && oppoEisExtensionEnabled();",
     "EIS2 disabled by default")
need(hwi, "setEis = oppoEisExtensionEnabled() && m_bEisEnable", "setEis gate")
need(hwi, "oppoEisExtensionEnabled() && m_bEisEnable &&", "DIS_ENABLE gate")
need(hwi, "Fixed-lens video", "fixed-lens comment")
need(hwi, "!logicalSessionPlan(videoLogicalPlan)", "MMS excludes logical sessions")
need(hwi, "gCamCapability[mCameraId]->position == CAM_POSITION_BACK", "MMS rear only")
need(hwi, "gCamCapability[mCameraId]->aux_cam_cap == NULL", "MMS non-AUX only")

# C1-C4: standard predicates only trigger on explicit org.lineageos modes.
need(hwi, "if (oppoNativeMode(settings) != OPPO_NATIVE_HDR) return false;",
     "explicit HDR predicate")
need(hwi, "if (oppoNativeMode(settings) != OPPO_NATIVE_LLHDR) return false;",
     "explicit LLHDR predicate")
need(hwi, "nativeMode != OPPO_NATIVE_MFNR && nativeMode != OPPO_NATIVE_SUPERPHOTO",
     "explicit MFNR/SUPERPHOTO predicate")
need(hwi, "if (oppoNativeMode(settings) != OPPO_NATIVE_DUAL_BOKEH) return false;",
     "explicit DUAL_BOKEH predicate")
need(hwi, "oppoNativeMode(meta) == OPPO_NATIVE_DUAL_BOKEH",
     "paired capture binding gate")
absent(hwi, "ANDROID_NOISE_REDUCTION_MODE_HIGH_QUALITY)) return false;",
       "implicit NR HQ multi-frame mapping")

# C5: standard templates carry no vendor keys.
need(hwi, "Standard templates carry no OPPO keys", "template comment")
absent(hwi, "defaultStrength = isVerifiedPairedSession()", "template beauty default")
absent(hwi, "settings.update(QCAMERA3_NATIVE_ALGORITHM_MODE", "template stillMode")
absent(hwi, "settings.update(QCAMERA3_NATIVE_ALGORITHM_BEAUTY_STRENGTH", "template beautyStrength")

# C6: vendor HDR/NIGHT scene announcements removed.
absent(hwi, "avail_scene_modes[index] = ANDROID_CONTROL_SCENE_MODE_HDR",
       "vendor HDR scene announcement")
absent(hwi, "avail_scene_modes[index] = ANDROID_CONTROL_SCENE_MODE_NIGHT",
       "vendor NIGHT scene announcement")
need(hwi, "The vendor HDR/NIGHT flows are explicit stillMode extensions now",
     "scene announcement comment")

# B: front beauty protocol + consumer wiring.
need(hwi, "static bool oppoFrontBeautySupported", "front beauty capability helper")
need(hwi, "nativeModes.add(OPPO_NATIVE_FRONT_BEAUTY)", "front beauty mode announcement")
need(hwi, "basicKeys.add(QCAMERA3_NATIVE_ALGORITHM_FRONT_BEAUTY_STRENGTH)",
     "front beauty strength request key")
need(hwi, "persist.vendor.camera.oppo.front_beauty.enable", "front beauty opt-in gate")
need(hwi, "mNativeFrontBeautyStrength", "front beauty strength capture")
need(hwih, "getNativeFrontBeautyStrength", "front beauty strength getter")
need(channel, "settings->native_front_beauty_strength", "front beauty settings copy")
need(header, "native_front_beauty_strength", "jpeg settings field")
need(post, '"FrontBeauty"', "front beauty trace")
need(bp, "QCamera2/HAL3/QCamera3FrontBeauty.cpp", "HAL module source")
need(HAL3 / "QCamera3FrontBeauty.h", "ARC_BS_IMAGE_Process", "adapter ABI comment")
need(HAL3 / "QCamera3FrontBeauty.cpp", "ARC_BS_IMAGE_SetFeatureLevel", "adapter symbol")
need(HAL3 / "QCamera3FrontBeauty.cpp", "kYv12Format = 0x605", "evidenced image format")

# Unsupported-tag list keeps the feature array rejected but not the strength key.
unsupported = re.search(r"unsupportedTags\[\] = \{(.*?)\n    \};", read(hwi), re.S)
checks += 1
if not unsupported:
    failures.append("QCamera3HWI.cpp: unsupportedTags[] block not found")
else:
    block = unsupported.group(1)
    if "QCAMERA3_NATIVE_ALGORITHM_FRONT_BEAUTY_STRENGTH" in block:
        failures.append("unsupportedTags[] still rejects FRONT_BEAUTY_STRENGTH")
    if "QCAMERA3_NATIVE_ALGORITHM_FRONT_BEAUTY_FEATURES" not in block:
        failures.append("unsupportedTags[] no longer rejects FRONT_BEAUTY_FEATURES")

# Automatic still policy: decoded preview statistics + failure diagnostics.
need(hwi, "void QCamera3HardwareInterface::updateAutoCaptureStats", "preview stats decode")
need(hwi, "void QCamera3HardwareInterface::fillAutoCaptureStats", "policy stats fill")
need(hwi, "CAM_INTF_META_STATS_HISTOGRAM_MODE", "luma histogram request")
need(hwi, "mAutoCaptureStatsEnabled", "auto stats enable flag")
need(hwi, "histogramClipping", "histogram clipping helper")
need(hwi, "void QCamera3HardwareInterface::noteNativeStageFailure", "failure trace helper")
need(hwi, "fillAutoCaptureStats(stats, settings);", "policy uses decoded stats")
need(hwi, "request->input_buffer != NULL || m_bIsVideo", "HDR predicate excludes video")
need(hwi, "request->input_buffer || m_bIsVideo ||", "LLHDR predicate excludes video")
need(hwi, "QCAMERA3_NATIVE_ALGORITHM_SCENE_STRENGTH", "scene strength announcement")
need(hwi, "noteNativeStageFailure(local.frame_number, requestedMode, -ENOTSUP)",
     "requested-but-not-executed trace")
need(hwi, "noteNativeStageFailure(single.frame_number, requestedMode, failureStatus)",
     "single-frame fallback trace")
need(hwi, "noteNativeStageFailure(frameNumber, OPPO_NATIVE_DUAL_BOKEH, bindStatus)",
     "paired bind failure trace")
need(hwi, "cannot be executed must not be reported as applied",
     "auto trigger availability guard")
need(hwi, "config.hdrEnable = autoHdr.count == 1", "auto policy per-request config")
need(hwi, "mAutoCaptureStats = AutoCaptureStats();", "stats reset per session")
need(hwi, "RAW16 must", "RAW16 admission comment")
need(hwi, "Explicit MFNR/SUPER_PHOTO cannot run its pipeline",
     "MFNR/SUPER single-frame degrade")
need(hwi, "The auto extension requests histogram statistics internally",
     "internal histogram result keys suppressed")
absent(hwi, "decision->raw16Available == 1", "inconsistent RAW16 admission")
need(post, "always publish the stage", "scene stage always recorded")
need(HAL3 / "QCamera3AutoCapturePolicy.cpp",
     "QCamera3AutoCapturePolicy::histogramClipping", "histogram helper implementation")
need(HAL3 / "QCamera3AutoCapturePolicy.h", "histogramClipping", "histogram helper declaration")

# Tests exist and are Python-parseable.
tests = CAMERA / "tests"
for name in ("run_front_beauty_tests.py", "run_auto_capture_tests.py", "hal_static_checks.py"):
    need(tests / name, "def main", "host test runner")
need(tests / "auto_capture_policy_test.cpp", "kReasonDynamicRange", "auto policy host test")
for path in sorted(tests.glob("*.py")):
    checks += 1
    try:
        compile(path.read_text(), str(path), "exec")
    except SyntaxError as exc:
        failures.append(f"{path.name}: python syntax error: {exc}")

if failures:
    print("static checks FAILED:")
    for item in failures:
        print(" -", item)
    sys.exit(1)
print(f"static checks passed ({checks} checks)")
sys.exit(0)
