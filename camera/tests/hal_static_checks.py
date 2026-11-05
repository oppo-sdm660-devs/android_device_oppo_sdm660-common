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

# Original HAL1 platform layer wiring (task-54).
tests = CAMERA / "tests"
platform_hwi = HAL3 / "QCamera3OppoPlatformHwi.cpp"
need(platform_hwi, "processStillFrame", "per-mode platform entry")
need(platform_hwi, "sessionAlgorithmMask", "platform register mask")
need(platform_hwi, "persist.vendor.camera.oppo.platform_hwi.run", "explicit run gate")
need(platform_hwi, "persist.vendor.camera.oppo.platform_hwi.update", "updateparameters gate")
need(post, "processStillFrame", "PostProc uses the platform entry")
need(post, "ArcFaceBeauty", "original face-beauty stage name")
need(post, "VDSces", "original SCES stage name")
need(HAL3 / "QCamera3OppoPlatformParams.h", "maskForStillMode", "reversed mask mapping")
need(HAL3 / "QCamera3OppoPlatformParams.h", "kFaceInfoBase", "reversed face-info block")
need(HAL3 / "QCamera3OppoPlatformParams.cpp", "kAlgoVdSces", "SCES algorithm bit")
need(CAMERA / "Android.bp", "QCamera3OppoPlatformParams.cpp", "params module in HAL")
need(tests / "fake_oppo_platform_hwi.cpp", "fake_oppo_platform_init", "fake platform lib")
need(tests / "platform_hwi_host_test.cpp", "processMask", "platform host test")

# Vendor-only ZSL path (task-59): reuse the stock HAL ZSL snapshot channel.
policy_h = HAL3 / "QCamera3ZslPolicy.h"
policy_cpp = HAL3 / "QCamera3ZslPolicy.cpp"
need(policy_h, "QCamera3ZslPolicy", "zsl policy class")
need(policy_h, "kFallbackUnsupported", "zsl fallback reasons")
need(policy_cpp, "classifySelection", "zsl selection classification")
need(policy_cpp, "standardEnableZsl", "zsl requires both triggers")
need(hwi, "needZSLCapture(const camera3_capture_request_t *request,", "zsl policy hook")
need(hwi, "readOppoZslProperties", "zsl properties at configureStreams")
need(hwi, "mHALZSL = CAM_HAL3_ZSL_TYPE_SNAPSHOT;", "reuse stock HAL ZSL snapshot")
need(hwi, "persist.vendor.camera.oppo.zsl.enable", "zsl master gate")
need(hwi, "persist.vendor.camera.oppo.zsl.max_age_ms", "zsl age property")
need(hwi, "persist.vendor.camera.oppo.zsl.max_bytes", "zsl memory cap")
need(hwi, "QCAMERA3_ZSL_APPLIED", "zsl result key applied")
need(hwi, "QCAMERA3_ZSL_FRAME_AGE_MS", "zsl result key age")
need(hwi, "QCAMERA3_ZSL_FRAME_NUMBER", "zsl result key frame")
need(hwi, "QCAMERA3_ZSL_FALLBACK_REASON", "zsl result key reason")
need(hwi, "noteZslSelection", "zsl selection bookkeeping")
need(hwi, "QCAMERA3_ZSL_APPLIED", "zsl result advertisement")
need(HAL3 / "QCamera3Channel.cpp", "noteZslSelection", "zsl age reporting")
need(HAL3 / "QCamera3Channel.cpp", "QCamera3ZslPolicy::kFallbackChannelError",
     "zsl channel error reporting")
need(HAL3 / "QCamera3VendorTags.cpp", '"org.lineageos.camera.zsl"', "zsl vendor section")
need(HAL3 / "QCamera3VendorTags.cpp", '"fallbackReason"', "zsl fallback reason tag")
need(CAMERA / "Android.bp", "QCamera3ZslPolicy.cpp", "policy module in HAL")
need(tests / "zsl_policy_test.cpp", "kFallbackAge", "zsl policy host test")

# task-65 build-risk audit: platform scratch (frame ctx +0x2c8/2cc/2d4) and the
# RE third batch constraints (HDR/LLHDR must not be routed to the platform block).
params_h = HAL3 / "QCamera3OppoPlatformParams.h"
params_cpp = HAL3 / "QCamera3OppoPlatformParams.cpp"
platform_hwi_h = HAL3 / "QCamera3OppoPlatformHwi.h"
for text, note in (
        ("kImagePtr = 0x00", "ctx image ptr"),
        ("kImageFormat = 0x30", "ctx image format"),
        ("kImageValid = 0x38", "ctx image valid flag"),
        ("kImageCount = 0x280", "ctx image count"),
        ("kScratchFd = 0x2C8", "ctx scratch fd"),
        ("kScratchPtr = 0x2CC", "ctx scratch ptr"),
        ("kScratchSize = 0x2D4", "ctx scratch size"),
        ("kFormatYuv420Nv21 = 2", "NV21 cam_format"),
        ("kScratchBytes = 0x1000", "scratch size")):
    need(params_h, text, note)
need(params_cpp, "FrameCtxOffsets::kImageCount, 1", "ctx image count filled")
need(params_cpp, "FrameCtxOffsets::kScratchPtr", "ctx scratch ptr filled")
need(platform_hwi, "ensureScratchLocked", "scratch allocation")
need(platform_hwi, "posix_memalign(&scratch, kOppoHwiScratchAlign", "aligned scratch")
need(platform_hwi, "memfd_create(\"oppo_hwi_scratch\"", "scratch memfd")
need(platform_hwi, "freeScratchLocked", "scratch release")
need(platform_hwi, "refusing platform path", "no-scratch fail closed")
need(platform_hwi, "oppo_platform::buildFrameCtx(mFrameCtx", "probe ctx uses full layout")
need(platform_hwi_h, "mScratch = nullptr", "scratch members")
need(tests / "platform_hwi_host_test.cpp", "FrameCtxOffsets::kScratchPtr",
     "platform host test scratch fields")
need(tests / "platform_hwi_host_test.cpp", "FrameCtxOffsets::kImagePtr",
     "platform host test image ptr")
need(tests / "platform_hwi_host_test.cpp", "readU8(ctx, FrameCtxOffsets::kImageValid)",
     "platform host test valid flag")
# task-63: only SCES(9)/front beauty(10)/rear beauty(5) are wired to platform.
# HDR/LLHDR/MFNR/SUPER/bokeh must never enter it.
params_h_text = read(params_h)
need(params_h, "kSupportedPlatformMask", "wired platform mask")
need(params_h, "isPlatformStillMode", "platform mode whitelist")
need(params_cpp, "isPlatformStillMode(int32_t stillMode)", "platform mode whitelist impl")
need(platform_hwi, "oppo_platform::isPlatformStillMode(stillMode)", "whitelist enforced")
need(platform_hwi, "return kSupportedPlatformMask;", "session registers wired bits only")
post_text = read(post)
for mode in ("OPPO_NATIVE_HDR", "OPPO_NATIVE_LLHDR", "OPPO_NATIVE_MFNR",
             "OPPO_NATIVE_SUPERPHOTO", "OPPO_NATIVE_DUAL_BOKEH"):
    checks += 1
    if f"processStillFrame(\n                    {mode}" in post_text or \
            f"processStillFrame({mode}" in post_text:
        failures.append(f"QCamera3PostProc.cpp: {mode} must not enter the platform block")
checks += 3
if post_text.count("processStillFrame(") != 3:
    failures.append("QCamera3PostProc.cpp: platform block must be SCENE+FRONT_BEAUTY+REAR_BEAUTY")
if "OPPO_NATIVE_REAR_BEAUTY, platformFrame" not in post_text:
    failures.append("QCamera3PostProc.cpp: rear beauty is not wired to platform")
if '"FaceBeauty"' not in post_text:
    failures.append("QCamera3PostProc.cpp: original rear-beauty trace name missing")
# Rear beauty platform-first, StockBeauty only as fallback (no double processing).
checks += 2
if "rearHandledByPlatform" not in post_text or "if (!rearHandledByPlatform)" not in post_text:
    failures.append("QCamera3PostProc.cpp: rear beauty fallback is not exclusive")
if post_text.count("QCamera3StockBeauty::process") != 1:
    failures.append("QCamera3PostProc.cpp: stock beauty must run only in the fallback")
# ZSL past frames must not enter any platform algorithm.
checks += 1
if post_text.count("!jpeg_settings->zsl_snapshot") != 3:
    failures.append("QCamera3PostProc.cpp: missing ZSL/platform mutual exclusion")

# task-65 build risks: explicit errno include and no memset on ZslFrameState.
need(hwi, "#include <errno.h>", "errno include for strtoll")
absent(hwi, "memset(mZslStates", "class-memaccess on non-trivial state")
need(hwi, "for (size_t i = 0; i < kZslStateSlots; ++i) mZslStates[i] = ZslFrameState();",
     "state reset by value assignment")
need(hwih, "QCamera3ZslPolicy.h", "zsl policy include")
need(policy_h, "kDefaultMaxAgeMs = 120", "zsl default age 120ms")

# BLOB ZSL success annotation must live in QCamera3PicChannel::ZSLChannelCb, not
# only in the (vendor YUV ZSL) QCamera3YUVChannel callback.
channel_text = read(channel)
pic_start = channel_text.find("void QCamera3PicChannel::ZSLChannelCb(")
yuv_start = channel_text.find("void QCamera3YUVChannel::ZSLChannelCb(")
if pic_start < 0:
    failures.append("QCamera3Channel.cpp: missing QCamera3PicChannel::ZSLChannelCb")
else:
    pic_end = channel_text.find("\n}\n", pic_start)
    pic_body = channel_text[pic_start:pic_end]
    checks += 2
    if "isOppoZslFrame" not in pic_body or "noteZslSelection" not in pic_body:
        failures.append("QCamera3PicChannel::ZSLChannelCb: missing success annotation")
    if "CAM_INTF_META_SENSOR_TIMESTAMP" not in pic_body:
        failures.append("QCamera3PicChannel::ZSLChannelCb: missing past-frame timestamp")
checks += 1
if yuv_start < 0 or yuv_start > pic_start:
    failures.append("QCamera3Channel.cpp: YUV ZSL callback ordering changed unexpectedly")

# Vendor tag append integrity: section count, tag count/types, no reordering.
vt_h = read(HAL3 / "QCamera3VendorTags.h")
vt_c = read(HAL3 / "QCamera3VendorTags.cpp")
import re as _re
section_body = _re.search(r"enum qcamera3_ext_section \{(.*?)\n\};", vt_h, _re.S)
zsl_tags = _re.search(r"QCAMERA3_ZSL_ENABLE[^\n]*\n(.*?)\n\s*QCAMERA3_ZSL_END", vt_h, _re.S)
zsl_info = _re.search(r"qcamera3_zsl\[[^\]]*\] = \{(.*?)\n\};", vt_c, _re.S)
checks += 4
if section_body is None:
    failures.append("QCamera3VendorTags.h: section enum not found")
elif "QCAMERA3_ZSL,\n    QCAMERA3_SECTIONS_END," not in section_body.group(1):
    failures.append("QCamera3VendorTags.h: ZSL is not the last appended section")
if zsl_tags is None:
    failures.append("QCamera3VendorTags.h: ZSL tag enum not found")
elif len(_re.findall(r"QCAMERA3_ZSL_", zsl_tags.group(1))) != 5:
    failures.append("QCamera3VendorTags.h: ZSL tag enum must add 5 tags after ENABLE")
if zsl_info is None:
    failures.append("QCamera3VendorTags.cpp: qcamera3_zsl tag_info not found")
else:
    if len(_re.findall(r'\{ "\w+", TYPE_', zsl_info.group(1))) != 6:
        failures.append("QCamera3VendorTags.cpp: qcamera3_zsl must have 6 entries")
for entry in ('{ "zslApplied", TYPE_BYTE }', '{ "frameNumber", TYPE_INT64 }',
              '{ "frameAgeMs", TYPE_INT32 }', '{ "fallbackReason", TYPE_INT32 }'):
    need(HAL3 / "QCamera3VendorTags.cpp", entry, "zsl result tag type")
for tag in ("QCAMERA3_ZSL_ENABLE", "QCAMERA3_ZSL_MAX_AGE_MS", "QCAMERA3_ZSL_APPLIED",
            "QCAMERA3_ZSL_FRAME_AGE_MS", "QCAMERA3_ZSL_FRAME_NUMBER",
            "QCAMERA3_ZSL_FALLBACK_REASON"):
    need(HAL3 / "QCamera3VendorTags.cpp", f"(uint32_t){tag}", "zsl tag registration")

# task-71 face-info: HAL3 CAM_INTF_META_FACE_DETECTION -> platform param face block.
for text, note in (
        ("kMaxFaceInfo = 10", "face info max count"),
        ("kFaceBlockOffset = 0x28", "face block offset"),
        ("kFaceArrayStride = 0x28", "face array stride"),
        ("kFaceCountOffset = 0x170", "face count offset"),
        ("buildFaceInfo", "face info builder")):
    need(params_h, text, note)
need(params_cpp, "buildFaceInfo(param, size, frame.faceInfo", "buildParam feeds face block")
need(platform_hwi_h, "QCamera3OppoPlatformFaceInfo", "platform face info type")
need(platform_hwi_h, "static void setFaceInfo", "face info cache setter")
need(platform_hwi_h, "static bool getFaceInfo", "face info cache getter")
need(platform_hwi, "kOppoFaceMaxAgeFrames", "bounded face cache age")
need(platform_hwi, "params.faceInfo.count = faceCount", "processStillFrame face conversion")
need(platform_hwi_h, "beautyParamsReady(int32_t stillMode)",
     "beauty params readiness accessor")
need(platform_hwi_h, "mBeautyModelRear", "rear beauty model path")
need(platform_hwi, "oppoHwiResolveModelPath", "beauty model stat resolution")
need(platform_hwi, "kOppoHwiBeautyModelProp", "beauty model property override")
need(platform_hwi, "beauty model path unavailable", "beauty fail-closed path")
need(platform_hwi, "params.beauty.enabled = true", "beauty params constructed")
need(platform_hwi, "params.beauty.strength = frame.beautyStrength",
     "beauty strength passed")
need(platform_hwi, "params.scesParam", "SCES param written")
need(params_h, "struct BeautyParams", "beauty params type")
need(params_h, "kBeautyModelPath = 0x0060", "beauty model path offset")
need(params_h, "kBeautyStrength = 0x0164", "beauty strength offset")
need(params_h, "kScesParam = 0x10BC", "SCES param offset")
need(params_cpp, "buildBeautyParams", "beauty param builder")
need(params_cpp, "case 5: return kAlgoFaceBeauty | kAlgoFaceInfo",
     "rear beauty mask includes faceInfo")
need(params_cpp, "case 10: return kAlgoArcFaceBeauty | kAlgoFaceInfo",
     "front beauty mask includes faceInfo")
need(params_h, "kAlgoFaceInfo;", "supported platform mask includes faceInfo")
need(platform_hwi, "beauty_model_front", "front beauty model property")
need(hwi, "QCamera3OppoPlatformHwi::setFaceInfo", "HWI result feeds face cache")
need(hwi, "CAM_INTF_META_FACE_DETECTION, metadata)", "HWI reads face detection")
need(hwi, "mCropRegionMapper.toActiveArray(rect.left, rect.top, rect.width,",
     "HWI maps faces to active array")
post_text = read(post)
checks += 4
if post_text.count("QCamera3OppoPlatformHwi::getFaceInfo") != 3:
    failures.append("QCamera3PostProc.cpp: platform paths must fetch face info")
# task-75 A: beauty must NOT be gated on face count (RE: face block is bokeh/picbest only).
if "platformFaces.count > 0" in post_text:
    failures.append("QCamera3PostProc.cpp: beauty must not gate on face count")
# task-75 D/E: beauty enabled per mode only when the model path is ready, with
# the user strength mapped into the platform frame; missing path falls back.
if post_text.count("beautyParamsReady(") != 2:
    failures.append("QCamera3PostProc.cpp: per-mode beauty readiness missing")
if post_text.count("platformFrame.beautyStrength") != 2:
    failures.append("QCamera3PostProc.cpp: beauty strength not passed")
if post_text.count("beauty model path unavailable") != 2:
    failures.append("QCamera3PostProc.cpp: beauty fallback log missing")
if "platformFrame.faceInfo = platformFaces;" not in post_text:
    failures.append("QCamera3PostProc.cpp: face info is not passed to platform frame")

# Tests exist and are Python-parseable.
for name in ("run_front_beauty_tests.py", "run_auto_capture_tests.py",
             "run_platform_hwi_tests.py", "run_zsl_policy_tests.py",
             "hal_static_checks.py"):
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
