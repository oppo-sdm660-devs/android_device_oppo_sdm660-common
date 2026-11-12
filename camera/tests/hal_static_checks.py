#!/usr/bin/env python3
"""Static protocol, dependency and evidenced ABI checks (not a build).

Verifies that the coupled edits stay coupled: standard-entry predicates, scene
mode announcement, template defaults, 4K/profile wiring, front beauty
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

# The paired vendor pipeline does not implement Camera2 physical output streams.
absent(hwi, "ANDROID_REQUEST_AVAILABLE_CAPABILITIES_LOGICAL_MULTI_CAMERA",
       "unsupported standard logical capability")
absent(hwi, "oppoPublishLogicalMetadata", "unsupported physical metadata publication")
for key in ("ANDROID_LOGICAL_MULTI_CAMERA_PHYSICAL_IDS",
            "ANDROID_LOGICAL_MULTI_CAMERA_SENSOR_SYNC_TYPE",
            "ANDROID_LOGICAL_MULTI_CAMERA_ACTIVE_PHYSICAL_ID",
            "ANDROID_REQUEST_AVAILABLE_PHYSICAL_CAMERA_REQUEST_KEYS"):
    need(hwi, f"info.erase({key});", f"remove unsupported physical metadata {key}")
need(hwi, "nativeModes.add(OPPO_NATIVE_DUAL_BOKEH)", "preserve real paired vendor capture")
need(hwi, "basicKeys.add(QCAMERA3_NATIVE_ALGORITHM_PORTRAIT_FRAMING)",
     "paired vendor framing request")
need(hwi, "basicKeys.add(QCAMERA3_NATIVE_ALGORITHM_PREVIEW_BOKEH_STRENGTH)",
     "paired vendor preview request")
need(hwi, "CAMERA_DEVICE_API_VERSION_3_5", "session parameter contract version")
need(hwi, "ANDROID_REQUEST_AVAILABLE_SESSION_KEYS", "public session parameter manifest")

# task-124: no fake reprocessing capabilities; CameraX must not select
# YUV-reprocessing ZSL. Pseudo-ZSL HAL path is off unless explicitly enabled.
absent(hwi, "ANDROID_REQUEST_AVAILABLE_CAPABILITIES_PRIVATE_REPROCESSING",
       "private reprocessing capability advertisement")
absent(hwi, "ANDROID_REQUEST_AVAILABLE_CAPABILITIES_YUV_REPROCESSING",
       "YUV reprocessing capability advertisement")
absent(hwi, "ANDROID_REPROCESS_MAX_CAPTURE_STALL, &max_stall_duration",
       "reprocess stall metadata")
absent(hwi, "io_format_map", "reprocess input/output format map")
absent(hwi, "max_input_streams = 1", "input stream count 1")
need(hwi, "const bool reprocess = false;", "reprocess metadata policy disabled")
need(hwi, "ZSL fill request failed rc=%d frame=%u",
     "ZSL super-buffer keepalive request")
need(channel, "ZSL request cam=%x rawCh=%x frame=%u num=%u",
     "ZSL request handles/frame log")
need(channel, "ZSL cb buf[%u] type=%d idx=%d frame_idx=%d flags=%x",
     "ZSL callback buffer details")
need(channel, "oppoPreviewFrameCachePut", "preview YUV frame cache")
need(channel, "oppoPreviewFrameCacheGet", "preview YUV frame lookup")
need(hwi, "return true;", "standard past-frame route enabled")
need(hwi, "camera %u capability is NULL; refusing to init HAL object",
     "constructor capability null guard")
need(channel, "plain snapshot request frame=%u rc=%d",
     "plain snapshot asks daemon for the frame")
need(channel, "snapshot buffer invalid: errFlag=%d zeroSource=%d",
     "zero/error snapshot source is rejected before JPEG")
need(hwi, 'property_get("persist.vendor.camera.halzsl.enable", prop, "1")',
     "pseudo-ZSL transitional default on")

# A. 4K property wiring and genuine profile-based stabilization availability.
for prop in ("persist.vendor.camera.oppo.video.allow_4k=1",
             "persist.vendor.camera.oppo.video.picture_overlap=1",
             "persist.vendor.camera.oppo.eis.extension=0",
             "persist.vendor.camera.oppo.vstab.mms=0"):
    need(VENDOR_PROP, prop, "device property")
absent(VENDOR_PROP, "vstab.publish", "removed publish property")
need(hwi, "persist.vendor.camera.oppo.video.allow_4k", "4K gate")
need(hwi, "persist.vendor.camera.oppo.eis.extension", "EIS2 extension gate")
absent(hwi, "oppoMmsVstabEnabled", "obsolete global MMS permission gate")
need(hwi, "QCamera3MmsChannel::probeProfile", "MMS real engine/profile probe")
need(hwi, "QCamera3MmsGyroSource::probe", "MMS actual sensor identity probe")

# MMS consumes the standard request; legacy EIS2 stays an explicit diagnostic.
absent(hwi, "oppoVstabPublishEnabled", "removed publish helper")
need(hwi, "oppoMmsVstabSupported(gCamCapability[cameraId])", "probed ON announcement")
need(hwi, "availableVstabModes.add(ANDROID_CONTROL_VIDEO_STABILIZATION_MODE_ON)",
     "standard ON announcement after real probe")
need(hwi, "availableVstabModes.add(ANDROID_CONTROL_VIDEO_STABILIZATION_MODE_OFF)",
     "OFF remains available")
need(hwi, "ANDROID_CONTROL_VIDEO_STABILIZATION_MODE_PREVIEW_STABILIZATION",
     "accept values without enabling")
absent(hwi, "!oppoEisSupported(capability)", "old EIS availability rejection")
need(hwi, "uint8_t fwk_videoStab = ANDROID_CONTROL_VIDEO_STABILIZATION_MODE_OFF;",
     "standard result starts OFF without evidence")
need(hwi, "getVstabDiagnostics(internalFrameNumber, diagnostics)",
     "final metadata uses matching-frame diagnostics")
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
absent(hwi, "persist.vendor.camera.oppo.front_beauty.enable", "obsolete beauty global permission gate")
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
need(hwi, "RAW16/DNG admission is backed by real sensor RAW16/colour calibration",
     "RAW16 admission comment")
need(hwi, "Explicit MFNR/SUPER_PHOTO cannot run its pipeline",
     "MFNR/SUPER single-frame degrade")
need(hwi, "The auto extension requests histogram statistics internally",
     "internal histogram result keys suppressed")
absent(hwi, "decision->raw16Available == 1", "inconsistent RAW16 admission")
need(HAL3 / "QCamera3RawCapability.cpp", "DPC calibration is not exported",
     "RAW16 admission without a fabricated hot-pixel map")
need(HAL3 / "QCamera3RawCapability.cpp", "The ON hot-pixel mode stays unadvertised",
     "no fake ON hot-pixel map")
need(hwi, "RAW16 cannot be combined with video, ZSL or secure streams",
     "RAW stream mutual exclusion")
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
        ("kImagePtr = 0x04", "evidenced ctx image ptr"),
        ("kImageFormat = 0x30", "ctx image format"),
        ("kImageValid = 0x38", "ctx image valid flag"),
        ("kImageCount = 0x280", "ctx image count"),
        ("kScratchFd = 0x2C8", "ctx scratch fd"),
        ("kScratchPtr = 0x2CC", "ctx scratch ptr"),
        ("kScratchSize = 0x2D4", "ctx scratch size"),
        ("kFormatYuv420Nv21 = 2", "NV21 cam_format"),
        ("kScratchBytes = 0x1000", "scratch size")):
    need(params_h, text, note)
need(params_cpp, "FrameCtxOffsets::kImageCount, uint32_t(count)",
     "ctx image count filled")
need(params_cpp, "FrameCtxOffsets::kScratchPtr", "ctx scratch ptr filled")
need(platform_hwi, "ensureScratchLocked", "scratch allocation")
need(platform_hwi, "posix_memalign(&scratch, kOppoHwiScratchAlign", "aligned scratch")
need(platform_hwi, "memfd_create(\"oppo_hwi_scratch\"", "scratch memfd")
need(platform_hwi, "freeScratchLocked", "scratch release")
need(platform_hwi, "refusing platform path", "no-scratch fail closed")
need(platform_hwi, "oppo_platform::buildFrameCtx(mFrameCtx", "probe ctx uses full layout")
# task-100 P3: multi-image (current-capture bracket) frame ctx slots.
need(params_h, "kMaxFrameImages = 10", "multi-image limit")
need(params_h, "struct Image", "multi-image input type")
need(params_h, "kImageSlotStride = 0x40", "ctx image slot stride")
need(params_cpp, "FrameCtxOffsets::kImageSlotStride", "ctx slot loop")
need(platform_hwi_h, "kOppoPlatformMaxImages", "Hwi multi-image limit")
need(platform_hwi, "params.imageCount = count", "Hwi forwards multi-image count")
# task-100 P2: SUPER single-frame probe is property gated and dumpable.
need(platform_hwi, "persist.vendor.camera.oppo.platform_hwi.super", "SUPER probe property")
need(platform_hwi, "superProbeReady", "SUPER probe readiness")
need(platform_hwi, "SUPER probe needs platform_hwi.update=1", "SUPER update prerequisite")
need(platform_hwi, "\"super_pre\"", "SUPER pre dump")
need(platform_hwi, "\"super_post\"", "SUPER post dump")
need(platform_hwi_h, "mScratch = nullptr", "scratch members")
need(tests / "platform_hwi_host_test.cpp", "FrameCtxOffsets::kScratchPtr",
     "platform host test scratch fields")
need(tests / "platform_hwi_host_test.cpp", "FrameCtxOffsets::kImagePtr",
     "platform host test image ptr")
need(tests / "platform_hwi_host_test.cpp", "readU8(ctx, FrameCtxOffsets::kImageValid)",
     "platform host test valid flag")
# Explicit normalized pixel adapters own beauty/SCES processing; the old
# platform state machine remains an isolated diagnostic, covered by host tests.
params_h_text = read(params_h)
need(params_h, "kSupportedPlatformMask", "wired platform mask")
need(params_h, "isPlatformStillMode", "platform mode whitelist")
need(params_cpp, "isPlatformStillMode(int32_t stillMode)", "platform mode whitelist impl")
need(platform_hwi, "oppo_platform::isPlatformStillMode(stillMode)", "whitelist enforced")
need(platform_hwi, "kSupportedPlatformMask & ~kAlgoSuperPhoto",
     "session mask excludes SUPER unless probe enabled")
need(platform_hwi, "if (oppoHwiSuperProbeEnabled()) mask |= kAlgoSuperPhoto",
     "session registers SUPER only for the probe")
post_text = read(post)
for mode in ("OPPO_NATIVE_HDR", "OPPO_NATIVE_LLHDR", "OPPO_NATIVE_MFNR",
             "OPPO_NATIVE_DUAL_BOKEH"):
    checks += 1
    if f"processStillFrame(\n                    {mode}" in post_text or \
            f"processStillFrame({mode}" in post_text:
        failures.append(f"QCamera3PostProc.cpp: {mode} must not enter the platform block")
need(post, "processExplicitFrame", "isolated stock pixel adapter")
need(post, "processImageAdjustFrame", "isolated ImageAdjust pixel adapter")
need(platform_hwi_h, "imageAdjustAvailable", "ImageAdjust symbol probe API")
need(platform_hwi_h, "explicitAvailable", "explicit pixel capability probe API")
need(tests / "run_front_beauty_tests.py", "fake_arcsoft_beautyshot.cpp", "beauty behavior harness")
need(tests / "run_platform_hwi_tests.py", "fake_oppo_platform_hwi.cpp", "platform behavior harness")

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
need(platform_hwi, '{"/etc/camera/gasryuv.model", "etc-file"}',
     "task-79 first /etc candidate")
need(platform_hwi, '{"/system/etc/camera/gasryuv.model", "system-file"}',
     "task-79 /system file fallback")
need(platform_hwi, '{"/system/etc/camera", "system-dir"}', "task-79 /system dir fallback")
need(platform_hwi, '{"/vendor/etc/camera", "vendor-dir"}', "task-79 vendor dir fallback")
need(platform_hwi, "oppoHwiPathIsDirectory", "model path dir detection")
need(platform_hwi, "oppo hwi beauty model rear=%s (source=%s%s)",
     "model path source log")
need(platform_hwi, "beauty model path unavailable", "beauty fail-closed path")
need(platform_hwi, "params.beauty.enabled = true", "beauty params constructed")
need(platform_hwi, "params.beauty.strength = frame.beautyStrength",
     "beauty strength passed")
need(platform_hwi, "params.scesParam", "SCES param written")
# task-90: HAL ZSL snapshot defers the JPEG stream; its daemon drop must not be
# reported as a fatal buffer error for BLOB buffers on the ZSL picture channel.
need(policy_h, "suppressHalZslBlobDrop", "HAL ZSL drop classification helper")
need(policy_cpp, "suppressHalZslBlobDrop", "HAL ZSL drop classification impl")
need(hwi, "QCamera3ZslPolicy::suppressHalZslBlobDrop(",
     "HWI uses HAL ZSL drop classification")
need(hwi, "persist.vendor.camera.oppo.zsl.halzsl_drop_bypass",
     "HAL ZSL drop bypass property")
need(hwi, "HAL ZSL expected JPEG drop suppressed",
     "HAL ZSL expected drop log")
# PicChannel pseudo-ZSL drop path (same failure can arrive via the channel).
need(HAL3 / "QCamera3Channel.cpp", "suppressHalZslPseudoFrameDrop",
     "PicChannel pseudo-ZSL drop suppression")
need(HAL3 / "QCamera3Channel.cpp", "HAL ZSL expected pseudo-frame drop suppressed",
     "PicChannel expected drop log")
# Native stillMode capture on the ZSL channel: no past frame -> the mismatch
# branch must also be suppressed, otherwise the JPEG is dropped and the App
# times out (5s) for HDR/LLHDR/MFNR/SUPER/beauty/bokeh.
need(HAL3 / "QCamera3Channel.cpp", "HAL ZSL expected pseudo-frame mismatch suppressed",
     "PicChannel pseudo-ZSL frame mismatch suppression")
# task-104: native stillMode JPEG delivery. Standard stills always keep
# zsl_snapshot; native stills set it only when HWI routed the capture through the
# past-frame path (frame-specific pseudo-ZSL delivery loses the picture stream on
# the daemon, observed on device 2026-11-08).
need(HAL3 / "QCamera3Channel.cpp",
     "zslPastFrame || hal_obj->getNativeStillMode() == OPPO_NATIVE_OFF",
     "zsl_snapshot covers past-frame and standard still delivery")
# RE task-98 P2 fallback: plain snapshot channel for the session.
need(HAL3 / "QCamera3HWI.cpp", "persist.vendor.camera.oppo.platform_hwi.disable_halzsl",
     "disable_halzsl A/B property")
need(HAL3 / "QCamera3HWI.cpp", "forcing plain snapshot channel",
     "disable_halzsl forces plain snapshot channel")
# task-99: standard ENABLE_ZSL on a pseudo-ZSL session must not take the
# past-frame branch (no JPEG on current builds); stock error is kept for
# pseudo drops so a missing delivery fails fast instead of hanging 5s.
need(HAL3 / "QCamera3HWI.cpp", "oppoStandardZslPastFrameAllowed",
     "standard_past_frame A/B property")
need(HAL3 / "QCamera3HWI.cpp", "standard ENABLE_ZSL on pseudo-ZSL session",
     "standard ENABLE_ZSL declines past-frame on pseudo-ZSL session")
need(HAL3 / "QCamera3HWI.cpp", "isZSLCapture=%d halzsl=%d",
     "drop diagnostics include request/channel state")
# Native stillMode (REAR_BEAUTY/DUAL_BOKEH): device 2026-11-08 proved the
# default past-frame route never delivers their JPEG (only drop suppression then
# 3s deferred timeout); the frame-specific pseudo-ZSL route delivers once
# b6da5e7 suppresses its expected drop/buffer error. Default frame-specific;
# property 1 keeps the stock past-frame route for A/B only.
need(HAL3 / "QCamera3HWI.cpp", "persist.vendor.camera.oppo.zsl.native_past_frame",
     "native frame-specific/past-frame A/B property")
need(HAL3 / "QCamera3HWI.cpp",
     'property_get("persist.vendor.camera.oppo.zsl.native_past_frame", prop, "0")',
     "native frame-specific default")
need(HAL3 / "QCamera3HWI.cpp", "never delivers their JPEG",
     "native default comment records the device evidence")
need(HAL3 / "QCamera3HWI.cpp", "native stillMode=%d uses past-frame ZSL delivery",
     "native stillMode past-frame delivery log")
need(HAL3 / "QCamera3Channel.cpp",
     "zslPastFrame || hal_obj->getNativeStillMode() == OPPO_NATIVE_OFF",
     "zsl_snapshot marks past-frame or standard still delivery")
need(HAL3 / "QCamera3Channel.cpp", "CAM_HAL3_JPEG_TYPE_DEPTH, isZSL",
     "PicChannel passes isZSL to every jpeg setting")
need(HAL3 / "QCamera3Channel.cpp", "haveSnapshot && QCamera3ZslPolicy::suppressHalZslPseudoFrameDrop",
     "PicChannel suppresses a pseudo drop only when a snapshot is delivered")
need(policy_cpp, "suppressHalZslPseudoFrameDrop", "pseudo drop classification impl")
# task-104: pseudo-ZSL deferred JPEG drop/error must not surface as buffer lost;
# real PostProc failures still report through jpegEvtHandle and a real timeout is
# reported explicitly by the pending-buffer timeout path.
need(policy_h, "suppressPseudoZslJpegTransition", "pseudo-ZSL JPEG transition helper")
need(policy_cpp, "suppressPseudoZslJpegTransition", "pseudo-ZSL JPEG transition impl")
need(hwi, "(j->isZSL || pseudoZslJpegTransition ||",
     "metadata drop covers frame-specific pseudo-ZSL")
need(hwi, "snapshot JPEG buffer error deferred to jpeg result",
     "snapshot buffer error deferred to jpeg result")
need(hwi, "HAL plain snapshot JPEG drop suppressed",
     "plain snapshot metadata drop suppressed")
need(hwi, "HAL ZSL deferred JPEG timeout reported",
     "real deferred JPEG timeout still reported")
need(hwi, "persist.vendor.camera.oppo.zsl.halzsl_drop_bypass",
     "pseudo-ZSL transition bypass property")
need(hwi, "QCamera3ZslPolicy::suppressPseudoZslJpegTransition(",
     "HWI uses pseudo-ZSL transition helper")
channel_h = HAL3 / "QCamera3Channel.h"
need(channel_h, "virtual bool isSnapshotJpegChannel()", "JPEG channel marker on base")
# task-113 M1: standard-ZSL ring PoC must be opt-in and not alter delivery.
ring_policy = HAL3 / "QCamera3ZslRingPolicy.cpp"
need(HAL3 / "QCamera3ZslRingPolicy.h", "select(", "ring selection policy")
need(ring_policy, "averageFpsX100", "ring stats fps")
need(CAMERA / "Android.bp", "QCamera3ZslRingPolicy.cpp", "ring policy built")
need(hwi, "persist.vendor.camera.oppo.zsl.ring", "ring property name")
need(hwi, 'property_get("persist.vendor.camera.oppo.zsl.ring", prop, "0")',
     "ring property default off")
need(hwi, "ZSLRING gate: enabled=%d picture=%p isZSL=%d state=%d streamOnPending=%d",
     "ring gate log on the normal stream-on path")
need(hwi, "ZSLRING configure: HALZSL=%d picture=%p zslChannel=%p isZSL=%d",
     "ring configure-state log")
need(hwi, "mPictureChannel->startZslRing();", "ring started on normal path")
need(hwi, "allocZslRingFrameNumber", "ring frame number allocation")
need(channel, "handleZslRingSuperBuf", "ring super-buffer handler")
need(channel, "requestZslRingFrameLocked", "ring keep-alive request")
need(channel, "ZSLRING stats", "ring periodic stats log")
need(channel, "stopZslRing(\"channel_stop\")", "ring stops on channel stop")
need(channel_h, "mZslRingActive", "ring state in PicChannel")
need(CAMERA / "tests" / "zsl_ring_policy_test.cpp", "testSelection",
     "ring policy host test")
need(CAMERA / "tests" / "run_zsl_ring_tests.py", "zsl_ring_policy_test",
     "ring policy host runner")
need(CAMERA / "tests" / "check_jpeg_content.py", "JPEG content checker",
     "uniform/green JPEG rejection")
# task-118 M2: standard-ZSL delivery from a selected ring frame, normal result.
need(hwi, "prepareRingZslRequest", "HWI marks ring-selected still requests")
need(hwi, "ZSLRING dispatch request=%u (still BLOB, zslHint=%d, pseudo bypassed)",
     "ring dispatch log")
need(hwi, "takeZslRingSelection", "HWI takes selected source timestamp")
need(hwi, "ZSLRING result frame=%u source=%u ts=%lld", "ring result timestamp log")
need(channel, "persist.vendor.camera.oppo.zsl.ring.max_age_ms", "ring maxAge property")
need(channel, "oppoZslRingMaxAgeMs", "ring maxAge helper")
need(channel, "deliverRingZslFrame", "ring past-frame delivery")
need(channel, "ZSLRING select request=%u source=%u ts=%lld", "ring select log")
need(channel, "ZSLRING fallback live capture", "ring select fallback log")
need(channel, "releaseRingZslBuffer", "ring buffer returned after JPEG")
need(channel, "mRingZslDispatch", "ring request suppresses pseudo deferral")
need(channel, "!mRingZslDispatch", "zsl_snapshot is not set for ring requests")
# task-119 ring4: ring path bypasses pseudo ZSL accounting and fallback keeps ring.
need(hwi, "pseudo bypassed", "ring dispatch happens before needZSLCapture")
need(hwi, "ZSLRING skip (ring empty warmup)", "HWI ring warmup skip")
need(channel, "isPendingCaptureFrame",
     "ring super buffers routed away from pending capture frames")
need(channel, "checkZslRingStall", "ring stall watchdog")
need(hwi, "checkZslRingStall", "HWI drives ring stall watchdog")
need(channel, "cancel_super_buf_request", "daemon super-buffer cancel")
need(channel, "ZSLRING drain held=%u inFlight=%u", "ring drain buffer counts")
need(channel, "ZSLRING cancel request rc=%d count=%u", "ring cancel log")
need(channel, "ZSLRING stall: inFlight=%u held=%zu no cb for %lldms",
     "ring stall recovery log")
need(channel, "resumeZslRingAfterLive", "ring restart after live jpeg")
need(channel, "ZSLRING restart after live capture request=%u", "ring restart log")
need(hwi, "shouldStartZslRing", "HWI lazily starts ring when stopped")
need(hwi, "ZSLRING lazy start request=%u", "hwi lazy start log")
need(channel, "ZSLRING warmup live request=%u", "warmup live stop/restart log")
need(channel, "ZSLRING cb frame=%u await=%u pendingCapture=%d -> %s",
     "ring/capture callback routing log")
need(channel, "ZSLRING request frame=%u inFlight=%u", "ring request log")
need(channel, "ZSLRING cb bufs=%u imageIdx=%d type=%d len=%zu ts=%lld", "ring callback log")
need(channel, "ZSLRING entry frame=%u ts=%lld entries=%zu", "ring entry log")
need(hwi, "markZslRingRequest", "ring request marker")
need(hwi, "isZslRingRequest", "ring request marker lookup")
need(hwi, "ZSLRING metadata requested request=%u", "ring metadata requested")
need(hwi, "ZSLRING metadata queued request=%u", "ring metadata queued")
need(channel, "consumeRingZslFallback", "ring fallback marker")
need(channel, "ZSLRING stopped for non-ring capture request=%u",
     "non-ring capture stops/cancels ring")
need(channel, "ZSLRING fallback live capture request=%u failures=%u (ring kept)",
     "fallback keeps ring")
need(channel, "ZSLRING fallback live request=%u (ring restart after jpeg)",
     "fallback restarts ring after live JPEG")
need(channel, "ZSLRING disabled after %u consecutive fallbacks",
     "repeated fallback disables ring")
need(channel, "ZSLRING fallback live capture request=%u failures=%u",
     "fallback keeps ring / failure counter log")
need(channel, "ZSLRING select-times requestSensor=%lld now=%lld",
     "ring selection raw time-base log")
need(hwi, "ch->isSnapshotJpegChannel()) ||",
     "buffer-error suppression covers all snapshot JPEG channels")
# 272d031 native past-frame code path stays available as A/B (property 1), but is
# no longer the default (task-107 device evidence).
need(hwi, "persist.vendor.camera.oppo.zsl.native_past_frame",
     "native past-frame A/B property preserved")
need(hwi, "native stillMode=%d uses past-frame ZSL delivery",
     "native past-frame A/B routing preserved")
need(HAL3 / "QCamera3Channel.cpp", "zslPastFrame", "native zsl_snapshot flag preserved")
# 4K build-time fallback: persisted values from older ROMs may shadow vendor.prop.
need(HAL3 / "QCamera3HWI.cpp", "ro.vendor.camera.oppo.video.allow_4k",
     "4K ro fallback key")
need(HAL3 / "QCamera3HWI.cpp", "oppoPropIntPersistOrRo", "persist/ro property fallback")
need(CAMERA.parent / "vendor.prop", "ro.vendor.camera.oppo.video.allow_4k=1",
     "4K ro default in vendor.prop")
need(CAMERA.parent / "vendor.prop", "ro.vendor.camera.oppo.video.picture_overlap=1",
     "video overlap ro default in vendor.prop")
# Stock video-only sizes publish an explicit 30fps min duration. Without it the
# picture-table derivation matches 4096x2160 against a slow 12/16MP still entry
# and advertises ~15fps (device recorded 4096x2160 at ~17fps on 2026-11-08).
need(HAL3 / "QCamera3HWI.cpp", "persist.vendor.camera.oppo.video.stock_sizes",
     "stock video sizes A/B property")
need(HAL3 / "QCamera3HWI.cpp",
     'property_get("persist.vendor.camera.oppo.video.stock_sizes", prop, "1")',
     "stock video sizes default enabled")
need(HAL3 / "QCamera3HWI.cpp", "OPPO_STOCK_VIDEO_30FPS_NS 33333333LL",
     "stock video 30fps duration constant")
for dim in ("w == 2160 && h == 1080", "w == 3840 && h == 2160",
            "w == 4096 && h == 2160"):
    need(HAL3 / "QCamera3HWI.cpp", dim, f"stock video size whitelist: {dim}")
hwi_src = read(HAL3 / "QCamera3HWI.cpp")
checks += 2
stock_fn = hwi_src[hwi_src.index("static int64_t oppoMinDurationFor"):]
stock_fn = stock_fn[:stock_fn.index(
    "const char *QCamera3HardwareInterface::getOppoSensorName")]
if "oppoStockVideoSizesEnabled() && oppoIsStockVideo30FpsSize(dim)" not in stock_fn:
    failures.append("QCamera3HWI.cpp: stock video sizes must bypass picture derivation")
elif stock_fn.index("oppoStockVideoSizesEnabled() && oppoIsStockVideo30FpsSize(dim)") > \
        stock_fn.index("const size_t pictureCount"):
    failures.append("QCamera3HWI.cpp: stock video check must precede picture derivation")
need(platform_hwi_h, "sceneStrength", "SCES strength input")
need(platform_hwi, "oppo hwi sces strength=%d param=%u", "SCES strength log")
need(platform_hwi, "mScesParamOverrideValid", "SCES property override")
need(platform_hwi, "mScesParamOverrideValid ? mScesParam", "SCES override precedence")
need(params_h, "struct BeautyParams", "beauty params type")
need(params_h, "kBeautyModelPath = 0x0060", "beauty model path offset")
need(params_h, "kBeautyStrength = 0x0164", "beauty strength offset")
need(params_h, "kScesParam = 0x10BC", "SCES param offset")
need(params_cpp, "buildBeautyParams", "beauty param builder")
need(params_cpp, "case 5: return kAlgoFaceBeauty | kAlgoFaceInfo",
     "rear beauty mask includes faceInfo")
need(params_cpp, "case 10: return kAlgoArcFaceBeauty | kAlgoFaceInfo",
     "front beauty mask includes faceInfo")
need(params_h, "kAlgoSuperPhoto;", "supported platform mask includes SUPER probe")
need(platform_hwi, "beauty_model_front", "front beauty model property")
need(hwi, "QCamera3OppoPlatformHwi::setFaceInfo", "HWI result feeds face cache")
need(hwi, "CAM_INTF_META_FACE_DETECTION, metadata)", "HWI reads face detection")
need(hwi, "mCropRegionMapper.toActiveArray(rect.left, rect.top, rect.width,",
     "HWI maps faces to active array")
# Face payload layout and real six-level beauty behavior are exercised by
# platform_hwi_host_test/front_beauty_host_test, independent of helper/log layout.

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

# task-142: OSS DocPanorama backend prototype (no stock module, no vendor blob).
pano_h = HAL3 / "QCamera3OppoPanorama.h"
pano_cpp = HAL3 / "QCamera3OppoPanorama.cpp"
need(pano_h, "class QCamera3OppoPanorama", "OSS panorama backend header")
need(pano_h, "stockDirectionCode", "stock direction mapping")
need(pano_h, "addFrame", "frame input API")
need(pano_h, "stitch", "stitch output API")
for banned in ("#include <cutils/", "#include <utils/", "#include <dlfcn.h>",
               "#include <hardware/", "QCamera2HardwareInterface"):
    absent(pano_h, banned, "Android/stock dependency in pure-logic header")
absent(pano_cpp, "dlopen", "must not load the stock HAL module")
absent(pano_cpp, "camera.sdm660", "must not reference the stock HAL module")
need(pano_cpp, "QCamera3OppoPanorama::addFrame", "frame blend implementation")
need(pano_cpp, "QCamera3OppoPanorama::stitch", "stitch implementation")
need(pano_cpp, "stockDirectionCode", "direction code implementation")
need(tests / "oppo_panorama_backend_test.cpp", "testHorizontalPan", "panorama host test")
need(tests / "run_oppo_panorama_tests.py", "def main", "panorama host test runner")
# task-143a: HAL build/protocol/frame hook wiring for the OSS backend.
need(CAMERA / "Android.bp", "QCamera3OppoPanorama.cpp", "panorama backend in HAL build")
need(hwi, "PANORAMA_ENABLE", "panorama enable request")
need(hwi, "PANORAMA_CAPTURE", "panorama capture request")
need(hwi, "PANORAMA_CANCEL", "panorama cancel request")
need(hwi, "PANORAMA_STATE", "panorama state result")
need(hwi, "PANORAMA_FRAME_COUNT", "panorama frame count result")
need(hwi, "PANORAMA_FAILURE_REASON", "panorama failure result")
need(hwi, "capturePanorama(&local)", "panorama capture uses owned asynchronous BLOB path")
need(hwi, "result.sources.back().timestampNs", "panorama result timestamp belongs to retained source")
need(hwi, "pairSourceMetadata", "panorama source metadata is paired")
need(hwi, "camera3_jpeg_blob_t footer", "panorama BLOB footer")
need(hwi, "drainPanorama();", "panorama flush completion barrier")
need(channel, "oppoPanoramaSetFrameHook", "preview frame hook")
need(HAL3 / "QCamera3VendorTags.cpp", "panoramaSupported", "panorama vendor tag registration")

# Synthetic panorama JPEG encoder and asynchronous HWI BLOB delivery.
pano_jpeg_h = HAL3 / "QCamera3PanoramaJpeg.h"
pano_jpeg_cpp = HAL3 / "QCamera3PanoramaJpeg.cpp"
need(pano_jpeg_h, "class QCamera3PanoramaJpeg", "panorama JPEG encoder API")
need(pano_jpeg_h, "exifOrientation", "EXIF orientation API")
need(pano_jpeg_cpp, "jpeg_destination_mgr", "bounded libjpeg memory encoder")
need(pano_jpeg_cpp, "appendExifOrientation", "EXIF orientation segment")
need(CAMERA / "Android.bp", "QCamera3PanoramaJpeg.cpp", "panorama JPEG encoder in HAL build")
need(CAMERA / "Android.bp", "libjpeg", "libjpeg vendor dependency")
need(tests / "panorama_jpeg_encoder_test.cpp", "testValidOrientations", "panorama JPEG host test")
need(tests / "run_panorama_jpeg_tests.py", "def main", "panorama JPEG host runner")
need(hwi, "QCamera3PanoramaJpeg::replaceExif", "panorama real canvas/source EXIF")

# task-156: front Visidon selfie-panorama backend (not HWI-wired until task-157).
visi_h = HAL3 / "QCamera3OppoVisidonPanorama.h"
visi_cpp = HAL3 / "QCamera3OppoVisidonPanorama.cpp"
visi_jni_h = HAL3 / "visidon_jnienv.h"
visi_jni_cpp = HAL3 / "visidon_jnienv.cpp"
need(visi_h, "class QCamera3OppoVisidonPanorama", "Visidon backend API")
need(visi_h, "halFailureReason", "HAL panorama failure mapping")
need(visi_h, "testEngine", "host-test engine injection")
need(visi_h, "/vendor/lib/libVDSelfiePanorama.so", "absolute vendor library path")
absent(visi_cpp, "camera.sdm660", "must not load the stock HAL module")
absent(visi_cpp, "FindClass", "no JNI VM/class dependency")
absent(visi_cpp, "AttachCurrentThread", "no JNI VM dependency")
absent(visi_h, "#include <android/", "backend header must stay host-testable")
need(visi_jni_h, "class VisidonJniEnv", "fake JNIEnv table")
need(visi_jni_h, "slotError", "fail-closed unexpected-slot handling")
need(visi_jni_cpp, "kGetByteArrayElements", "implemented JNI slot")
need(visi_jni_cpp, "unsupportedSlot", "unimplemented JNI slots fail closed")
need(tests / "visidon_panorama_backend_test.cpp", "captureJpeg", "Visidon backend host test")
need(tests / "run_visidon_backend_tests.py", "def main", "Visidon backend host runner")
stub_bp = tests / "visidon_compat" / "Android.bp"
stub_src = tests / "visidon_compat" / "libjnigraphics_stub.cpp"
need(stub_bp, 'stem: "libjnigraphics"', "vendor libjnigraphics stub stem")
need(stub_bp, "-soname,libjnigraphics.so", "vendor libjnigraphics stub SONAME")
need(stub_bp, 'compile_multilib: "32"', "vendor libjnigraphics stub is 32-bit")
need(stub_bp, "vendor: true", "vendor libjnigraphics stub is vendor-only")
need(stub_src, "AndroidBitmap_getInfo", "stub symbol")
need(stub_src, "AndroidBitmap_lockPixels", "stub symbol")
need(stub_src, "AndroidBitmap_unlockPixels", "stub symbol")

# task-168 B: binned aux is upscaled to the stock ARC reference, never faked.
need(HAL3 / "QCamera3DualCapture.h", "sourceWidth", "dual source dimensions")
need(HAL3 / "QCamera3DualCapture.cpp", "copyScaled", "aux reference upscale")
need(HAL3 / "QCamera3DualCapture.cpp", "copyCropScaled", "portrait FOV crop")
need(HAL3 / "QCamera3DualCapture.cpp", "teleMaxFov", "FOV-derived portrait crop")
need(HAL3 / "QCamera3DualGeometry.cpp", "sourceWidth = sourceW", "bind actual aux dimensions")
need(CAMERA / "tests" / "dual_geometry_test.cpp", "testBindBinnedAux", "dual geometry host test")
need(CAMERA / "tests" / "run_dual_geometry_tests.py", "def main", "dual geometry host runner")

# task-178 A/B diagnostics: both gates default off; baseline path stays META/trigger.
need(hwi, 'persist.vendor.camera.oppo.roi.parampath", prop, "0"', "ROI param-path gate default off")
need(hwi, 'persist.vendor.camera.oppo.af.op", prop, "0"', "AF op gate default off")
need(hwi, "CAM_INTF_PARM_AF_ROI", "legacy AF ROI A/B")
need(hwi, "CAM_INTF_PARM_AEC_ROI", "legacy AEC ROI A/B")
need(hwi, "mCameraHandle->ops->do_auto_focus", "legacy AF op A/B")
need(hwi, "cancel_auto_focus", "legacy AF cancel A/B")
need(hwi, "mLegacyAfTrigger", "AF op request handoff")


# task-180: Android 16 Camera3FakeStream-only compatibility.
fake_header = HAL3 / "QCamera3FakeStreamRules.h"
need(fake_header, "FAKE_WIDTH = 320", "fake stream width constant")
need(fake_header, "FAKE_HEIGHT = 240", "fake stream height constant")
need(fake_header, "FAKE_FORMAT = 0x22", "fake stream IMPLEMENTATION_DEFINED")
need(fake_header, "FAKE_USAGE = 0x800", "fake stream HW_COMPOSER usage")
need(fake_header, "numStreams == 1", "fake stream single-stream guard")
need(hwi, '#include "QCamera3FakeStreamRules.h"', "fake stream rules included by HWI")
need(hwi, "QCamera3FakeStreamRules::isFakeStreamOnly(", "fake stream detection in configure")
need(hwi, "return acceptFakeStreamOnlyLocked(streamList);", "fake-only config accepted without daemon stream")
need(hwi, "acceptFakeStreamOnlyLocked(", "fake-only cleanup helper")
need(tests / "fake_stream_rules_test.cpp", "AOSP Camera3FakeStream-only configuration", "fake stream host test")
need(tests / "run_fake_stream_rules_tests.py", "def main", "fake stream host runner")


# Explicit 2x is a distinct current-capture full-FOV path, not stillMode SUPER(4).
need(hwi, "QCAMERA3_NATIVE_ALGORITHM_SUPER_ZOOM_SUPPORTED", "2x capability consumer")
need(hwi, "QCAMERA3_NATIVE_ALGORITHM_APPLIED_SUPER_ZOOM", "2x actual application result")
need(hwi, "zoomPlan.captureCrop", "private full-FOV burst crop")
need(hwi, "owned.deferredMetadata", "2x final metadata waits for pixel-stage result")
need(post, "QCamera3StockSuperPhoto::process2x", "four-frame stock 2x consumer")
need(post, "mHdrFullFieldOfView", "2x source crop verification")
need(post, "QCamera3DualCapture::resize(mHdrImages[0], destination, true, 0.5f)", "2x failure preserves requested framing")

if failures:
    print("static checks FAILED:")
    for item in failures:
        print(" -", item)
    sys.exit(1)
print(f"static checks passed ({checks} checks)")
sys.exit(0)
