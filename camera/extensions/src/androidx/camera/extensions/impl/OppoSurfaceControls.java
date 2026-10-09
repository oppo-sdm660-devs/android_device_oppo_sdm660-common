/* SPDX-License-Identifier: BSD-3-Clause */
package androidx.camera.extensions.impl;

import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CaptureRequest;
import android.hardware.camera2.CaptureResult;
import android.hardware.camera2.params.ColorSpaceTransform;
import android.hardware.camera2.params.DynamicRangeProfiles;
import android.hardware.camera2.params.RggbChannelVector;
import android.hardware.camera2.params.TonemapCurve;
import android.util.Range;
import android.util.Rational;
import android.util.Size;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

/** Client-visible surface for the round-2 HALv3 capabilities.
 *
 * Every new capability is reachable through the standard Extensions parameter
 * channel ({@code SessionProcessorImpl.setParameters}) plus standard Camera2
 * keys, exactly as the library already carries {@code EXTENSION_STRENGTH} and
 * the optional portrait controls. Standard Camera2 keys are used wherever base
 * Camera2 has an equivalent control (exposure/tone-map/color correction/zoom,
 * hot-pixel map mode, EXIF); a vendor {@code org.lineageos.camera.*} tag is used
 * only where no standard control exists. Tag names are frozen by
 * {@code camera/docs/HAL3_PORT_PLATFORM.md} section 3; this class must not
 * invent names. Wire schemas and bounds are documented in
 * {@code device-tree-nonsource-20261017/camera/docs/EXTENSIONS_SURFACE.md}.
 *
 * A parameter is advertised only when the extension mode, the camera
 * characteristics and the HAL's own advertised request keys all agree, and it
 * is rejected with an {@link IllegalArgumentException} otherwise. A parameter
 * is never silently accepted and dropped.
 */
public final class OppoSurfaceControls {
    private OppoSurfaceControls() {}

    /** Extension provider modes; index order of {@code OppoExtender.Mode}. */
    public static final int MODE_AUTO=0,MODE_RETOUCH=1,MODE_HDR=2,MODE_NIGHT=3,MODE_BOKEH=4;
    /** Native algorithm mode ids; contract section 2. */
    public static final int NATIVE_OFF=0,NATIVE_HDR=1,NATIVE_LLHDR=2,NATIVE_MFNR=3,NATIVE_SUPERPHOTO=4,
        NATIVE_REAR_BEAUTY=5,NATIVE_FILTER=6,NATIVE_DUAL_BOKEH=7,NATIVE_DUAL_RELIGHT=8,NATIVE_SCENE=9,
        NATIVE_FRONT_BEAUTY=10,NATIVE_SINGLE_BOKEH=11,NATIVE_PICBEST=12,NATIVE_IMAGE_ADJUST=13,
        NATIVE_SINGLE_RELIGHT=14,NATIVE_DOC=15,NATIVE_SCES=16,NATIVE_DOUBLE_EXPOSURE=17,
        NATIVE_LONG_EXPOSURE=18,NATIVE_AR_TRACK=19,NATIVE_REMOSAIC=20;
    /** {@code appMask} bits; contract section 3.1. */
    public static final int MASK_BEAUTY=1,MASK_FILTER=2,MASK_SCENE=4,MASK_WATERMARK=8,MASK_AR=16,ALL_MASKS=31;

    // ---- request controls: org.lineageos.camera.algorithms (contract 3.1) ----
    public static final CaptureRequest.Key<Integer> FRONT_BEAUTY_STRENGTH=new CaptureRequest.Key<>(
        "org.lineageos.camera.algorithms.frontBeautyStrength",Integer.class);
    public static final CaptureRequest.Key<Integer> PIC_BEST_FRAMES=new CaptureRequest.Key<>(
        "org.lineageos.camera.algorithms.picBestFrames",Integer.class);
    public static final CaptureRequest.Key<Integer> PIC_BEST_STRENGTH=new CaptureRequest.Key<>(
        "org.lineageos.camera.algorithms.picBestStrength",Integer.class);
    public static final CaptureRequest.Key<Integer> PIC_BEST_OPEN_EYE_ONLY=new CaptureRequest.Key<>(
        "org.lineageos.camera.algorithms.picBestOpenEyeOnly",Integer.class);
    public static final CaptureRequest.Key<Integer> BLURLESS_THRESHOLD=new CaptureRequest.Key<>(
        "org.lineageos.camera.algorithms.blurlessThreshold",Integer.class);
    public static final CaptureRequest.Key<Integer> FACE_INFO_MODE=new CaptureRequest.Key<>(
        "org.lineageos.camera.algorithms.faceInfoMode",Integer.class);
    public static final CaptureRequest.Key<int[]> IMAGE_ADJUST=new CaptureRequest.Key<>(
        "org.lineageos.camera.algorithms.imageAdjust",int[].class);
    public static final CaptureRequest.Key<Integer> SINGLE_BOKEH_STRENGTH=new CaptureRequest.Key<>(
        "org.lineageos.camera.algorithms.singleBokehStrength",Integer.class);
    public static final CaptureRequest.Key<Integer> SINGLE_BOKEH_SPOT_MODE=new CaptureRequest.Key<>(
        "org.lineageos.camera.algorithms.singleBokehSpotMode",Integer.class);
    public static final CaptureRequest.Key<float[]> SINGLE_BOKEH_SPOT=new CaptureRequest.Key<>(
        "org.lineageos.camera.algorithms.singleBokehSpot",float[].class);
    public static final CaptureRequest.Key<Integer> PORTRAIT_LIGHT_MODE=new CaptureRequest.Key<>(
        "org.lineageos.camera.algorithms.portraitLightMode",Integer.class);
    public static final CaptureRequest.Key<Integer> PORTRAIT_LIGHT_STRENGTH=new CaptureRequest.Key<>(
        "org.lineageos.camera.algorithms.portraitLightStrength",Integer.class);
    public static final CaptureRequest.Key<Integer> DOC_MODE=new CaptureRequest.Key<>(
        "org.lineageos.camera.algorithms.docMode",Integer.class);
    public static final CaptureRequest.Key<Integer> DOC_STRENGTH=new CaptureRequest.Key<>(
        "org.lineageos.camera.algorithms.docStrength",Integer.class);
    public static final CaptureRequest.Key<Integer> AUTO_HDR_ENABLE=new CaptureRequest.Key<>(
        "org.lineageos.camera.algorithms.autoHdrEnable",Integer.class);
    public static final CaptureRequest.Key<Integer> AUTO_NIGHT_ENABLE=new CaptureRequest.Key<>(
        "org.lineageos.camera.algorithms.autoNightEnable",Integer.class);
    public static final CaptureRequest.Key<Integer> DOUBLE_EXPOSURE_MODE=new CaptureRequest.Key<>(
        "org.lineageos.camera.algorithms.doubleExposureMode",Integer.class);
    public static final CaptureRequest.Key<Integer> DOUBLE_EXPOSURE_STRENGTH=new CaptureRequest.Key<>(
        "org.lineageos.camera.algorithms.doubleExposureStrength",Integer.class);
    public static final CaptureRequest.Key<Long> DOUBLE_EXPOSURE_TIME_NS=new CaptureRequest.Key<>(
        "org.lineageos.camera.algorithms.doubleExposureTimeNs",Long.class);
    public static final CaptureRequest.Key<Integer> LONG_EXPOSURE_ENABLE=new CaptureRequest.Key<>(
        "org.lineageos.camera.algorithms.longExposureEnable",Integer.class);
    public static final CaptureRequest.Key<Integer> REMOSAIC_ENABLE=new CaptureRequest.Key<>(
        "org.lineageos.camera.algorithms.remosaicEnable",Integer.class);
    public static final CaptureRequest.Key<byte[]> EXIF_COMMENT=new CaptureRequest.Key<>(
        "org.lineageos.camera.algorithms.exifComment",byte[].class);
    public static final CaptureRequest.Key<byte[]> WATERMARK_ASSET=new CaptureRequest.Key<>(
        "org.lineageos.camera.algorithms.watermarkAsset",byte[].class);
    public static final CaptureRequest.Key<byte[]> STICKER_ASSET=new CaptureRequest.Key<>(
        "org.lineageos.camera.algorithms.stickerAsset",byte[].class);
    public static final CaptureRequest.Key<float[]> STICKER_RECT=new CaptureRequest.Key<>(
        "org.lineageos.camera.algorithms.stickerRect",float[].class);
    public static final CaptureRequest.Key<Integer> MIRROR_PREVIEW=new CaptureRequest.Key<>(
        "org.lineageos.camera.algorithms.mirrorPreview",Integer.class);
    public static final CaptureRequest.Key<Integer> APP_POLICY_MODE=new CaptureRequest.Key<>(
        "org.lineageos.camera.algorithms.appPolicyMode",Integer.class);
    public static final CaptureRequest.Key<Integer> APP_MASK=new CaptureRequest.Key<>(
        "org.lineageos.camera.algorithms.appMask",Integer.class);
    public static final CaptureRequest.Key<int[]> FACE_PREVIEW_PARAM=new CaptureRequest.Key<>(
        "org.lineageos.camera.algorithms.facePreviewParam",int[].class);
    public static final CaptureRequest.Key<Integer> DEPTH_QUALITY=new CaptureRequest.Key<>(
        "org.lineageos.camera.algorithms.depthQuality",Integer.class);
    /** Existing native still-mode transport (modes 0..20); selectable values are
     * the ones an extension type does not already own. */
    public static final CaptureRequest.Key<Integer> STILL_MODE=new CaptureRequest.Key<>(
        "org.lineageos.camera.algorithms.stillMode",Integer.class);
    /** Existing still filter selection (mode 6) and its flip contract. */
    public static final CaptureRequest.Key<Integer> FILTER_TYPE=new CaptureRequest.Key<>(
        "org.lineageos.camera.algorithms.filterType",Integer.class);
    public static final CaptureRequest.Key<Integer> FILTER_FLIP_MODE=new CaptureRequest.Key<>(
        "org.lineageos.camera.algorithms.filterFlipMode",Integer.class);
    /** Existing balanced dual portrait lighting controls (native mode 8). */
    public static final CaptureRequest.Key<Integer> RELIGHT_STRENGTH=new CaptureRequest.Key<>(
        "org.lineageos.camera.algorithms.relightStrength",Integer.class);
    public static final CaptureRequest.Key<Integer> RELIGHT_AZIMUTH=new CaptureRequest.Key<>(
        "org.lineageos.camera.algorithms.relightAzimuth",Integer.class);
    public static final CaptureRequest.Key<Integer> RELIGHT_ELEVATION=new CaptureRequest.Key<>(
        "org.lineageos.camera.algorithms.relightElevation",Integer.class);

    // ---- request controls: org.lineageos.camera.stream (contract 3.2) ----
    public static final CaptureRequest.Key<Integer> SLOW_MOTION_RATIO=new CaptureRequest.Key<>(
        "org.lineageos.camera.stream.slowMotionRatio",Integer.class);
    public static final CaptureRequest.Key<Integer> VIDEO_HDR_PROFILE=new CaptureRequest.Key<>(
        "org.lineageos.camera.stream.videoHdrProfile",Integer.class);
    public static final CaptureRequest.Key<Float> LOGICAL_ZOOM_HINT=new CaptureRequest.Key<>(
        "org.lineageos.camera.stream.logicalZoomHint",Float.class);
    public static final CaptureRequest.Key<Integer> LOW_POWER_MODE=new CaptureRequest.Key<>(
        "org.lineageos.camera.stream.lowPowerMode",Integer.class);
    public static final CaptureRequest.Key<Integer> DEPTH_STREAM_ENABLED=new CaptureRequest.Key<>(
        "org.lineageos.camera.stream.depthStreamEnabled",Integer.class);
    public static final CaptureRequest.Key<Integer> IR_STREAM_MODE=new CaptureRequest.Key<>(
        "org.lineageos.camera.stream.irStreamMode",Integer.class);

    // ---- request controls: org.lineageos.camera.ar (contract 3.2) ----
    public static final CaptureRequest.Key<Integer> AR_TRACKING_ENABLE=new CaptureRequest.Key<>(
        "org.lineageos.camera.ar.arTrackingEnable",Integer.class);
    public static final CaptureRequest.Key<Integer> AR_TRACKING_MODE=new CaptureRequest.Key<>(
        "org.lineageos.camera.ar.arTrackingMode",Integer.class);

    // ---- request controls: org.lineageos.camera.postprocess (contract 3.2) ----
    /** Provider-owned decoration box: normalized ten-thousandths left/top/right/bottom. */
    public static final CaptureRequest.Key<int[]> WATERMARK_LAYOUT=new CaptureRequest.Key<>(
        "org.lineageos.camera.postprocess.watermarkLayout",int[].class);
    public static final CaptureRequest.Key<Integer> MIRROR=new CaptureRequest.Key<>(
        "org.lineageos.camera.postprocess.mirror",Integer.class);
    public static final CaptureRequest.Key<byte[]> EXIF_USER_COMMENT=new CaptureRequest.Key<>(
        "org.lineageos.camera.postprocess.exifUserComment",byte[].class);

    // ---- static characteristics keys (contract 3.1/3.2) ----
    public static final CameraCharacteristics.Key<int[]> AVAILABLE_DOC_MODES=new CameraCharacteristics.Key<>(
        "org.lineageos.camera.algorithms.availableDocModes",int[].class);
    public static final CameraCharacteristics.Key<Long> MAX_LONG_EXPOSURE_NS=new CameraCharacteristics.Key<>(
        "org.lineageos.camera.algorithms.maxLongExposureNs",Long.class);
    public static final CameraCharacteristics.Key<long[]> LONG_EXPOSURE_RANGE_NS=new CameraCharacteristics.Key<>(
        "org.lineageos.camera.algorithms.longExposureRangeNs",long[].class);
    public static final CameraCharacteristics.Key<int[]> AVAILABLE_REMOSAIC_SIZES=new CameraCharacteristics.Key<>(
        "org.lineageos.camera.algorithms.availableRemosaicSizes",int[].class);
    public static final CameraCharacteristics.Key<Byte> REMOSAIC_SUPPORTED=new CameraCharacteristics.Key<>(
        "org.lineageos.camera.algorithms.remosaicSupported",Byte.class);
    public static final CameraCharacteristics.Key<Byte> DEPTH_SUPPORTED=new CameraCharacteristics.Key<>(
        "org.lineageos.camera.algorithms.depthSupported",Byte.class);
    public static final CameraCharacteristics.Key<Byte> AR_TRACKING_SUPPORTED=new CameraCharacteristics.Key<>(
        "org.lineageos.camera.algorithms.arTrackingSupported",Byte.class);
    public static final CameraCharacteristics.Key<Integer> PROFESSIONAL_VER=new CameraCharacteristics.Key<>(
        "org.lineageos.camera.algorithms.professionalVer",Integer.class);
    public static final CameraCharacteristics.Key<Byte> HLG_VIDEO_SUPPORTED=new CameraCharacteristics.Key<>(
        "org.lineageos.camera.algorithms.hlgVideoSupported",Byte.class);
    public static final CameraCharacteristics.Key<int[]> AVAILABLE_SLOW_MOTION_RATIOS=new CameraCharacteristics.Key<>(
        "org.lineageos.camera.stream.availableSlowMotionRatios",int[].class);
    public static final CameraCharacteristics.Key<Byte> AR_SECTION_TRACKING_SUPPORTED=new CameraCharacteristics.Key<>(
        "org.lineageos.camera.ar.arTrackingSupported",Byte.class);

    // ---- result keys (contract 3.1/3.2) ----
    public static final CaptureResult.Key<byte[]> NATIVE_TRACE=new CaptureResult.Key<>(
        "org.lineageos.camera.algorithms.nativeTrace",byte[].class);
    public static final CaptureResult.Key<int[]> FACE_ATTRIBUTES=new CaptureResult.Key<>(
        "org.lineageos.camera.algorithms.faceAttributes",int[].class);
    public static final CaptureResult.Key<Long> ESTIMATED_SNAPSHOT_EXP_TIME=new CaptureResult.Key<>(
        "org.lineageos.camera.algorithms.estimatedSnapshotExpTime",Long.class);
    public static final CaptureResult.Key<Byte> HOT_PIXEL_MAP_AVAILABLE=new CaptureResult.Key<>(
        "org.lineageos.camera.algorithms.hotPixelMapAvailable",Byte.class);
    public static final CaptureResult.Key<Integer> APPLIED_NATIVE_MODE=new CaptureResult.Key<>(
        "org.lineageos.camera.algorithms.appliedNativeMode",Integer.class);
    public static final CaptureResult.Key<long[]> VIDEO_SCENE_TIMELINE=new CaptureResult.Key<>(
        "org.lineageos.camera.diagnostics.videoSceneTimeline",long[].class);
    public static final CaptureResult.Key<int[]> STAGE_RECORDS=new CaptureResult.Key<>(
        "org.lineageos.camera.diagnostics.stageRecords",int[].class);
    public static final CaptureResult.Key<Integer> HARDWARE_LEVEL_ADVERTISED=new CaptureResult.Key<>(
        "org.lineageos.camera.diagnostics.hardwareLevelAdvertised",Integer.class);
    public static final CaptureResult.Key<float[]> AR_POSE=new CaptureResult.Key<>(
        "org.lineageos.camera.ar.arPose",float[].class);
    public static final CaptureResult.Key<Float> AR_POSE_CONFIDENCE=new CaptureResult.Key<>(
        "org.lineageos.camera.ar.arPoseConfidence",Float.class);

    /** Maximum byte length of every UTF-8 asset/comment payload. */
    public static final int MAX_TEXT_BYTES=256;
    /** Upper bound of the provider decoration box, matching the overlay schema. */
    public static final int DECORATION_SCALE=10000;

    private static final List<CaptureRequest.Key<?>> ALL_REQUESTS=Collections.unmodifiableList(Arrays.asList(
        FRONT_BEAUTY_STRENGTH,PIC_BEST_FRAMES,PIC_BEST_STRENGTH,PIC_BEST_OPEN_EYE_ONLY,BLURLESS_THRESHOLD,
        FACE_INFO_MODE,IMAGE_ADJUST,SINGLE_BOKEH_STRENGTH,SINGLE_BOKEH_SPOT_MODE,SINGLE_BOKEH_SPOT,
        PORTRAIT_LIGHT_MODE,PORTRAIT_LIGHT_STRENGTH,DOC_MODE,DOC_STRENGTH,AUTO_HDR_ENABLE,AUTO_NIGHT_ENABLE,
        DOUBLE_EXPOSURE_MODE,DOUBLE_EXPOSURE_STRENGTH,DOUBLE_EXPOSURE_TIME_NS,LONG_EXPOSURE_ENABLE,
        REMOSAIC_ENABLE,EXIF_COMMENT,WATERMARK_ASSET,STICKER_ASSET,STICKER_RECT,MIRROR_PREVIEW,
        APP_POLICY_MODE,APP_MASK,FACE_PREVIEW_PARAM,DEPTH_QUALITY,STILL_MODE,FILTER_TYPE,FILTER_FLIP_MODE,
        RELIGHT_STRENGTH,RELIGHT_AZIMUTH,RELIGHT_ELEVATION,SLOW_MOTION_RATIO,
        VIDEO_HDR_PROFILE,LOGICAL_ZOOM_HINT,LOW_POWER_MODE,DEPTH_STREAM_ENABLED,IR_STREAM_MODE,
        AR_TRACKING_ENABLE,AR_TRACKING_MODE,WATERMARK_LAYOUT,MIRROR,EXIF_USER_COMMENT));
    /** Standard ANDROID_SHARPNESS request/result. The SDK hides the constant,
     * so the frozen QName is resolved through the public typed-key constructor,
     * exactly like the hidden face fields in {@link OppoExtender}. */
    public static final CaptureRequest.Key<Integer> SHARPNESS=new CaptureRequest.Key<>(
        "android.sharpness",Integer.class);
    public static final CaptureResult.Key<Integer> SHARPNESS_RESULT=new CaptureResult.Key<>(
        "android.sharpness",Integer.class);

    /** Standard Camera2 controls the surface validates itself. */
    private static final List<CaptureRequest.Key<?>> STANDARD_SURFACE=Collections.unmodifiableList(Arrays.asList(
        CaptureRequest.CONTROL_ZOOM_RATIO,CaptureRequest.TONEMAP_MODE,CaptureRequest.TONEMAP_CURVE,
        SHARPNESS,CaptureRequest.STATISTICS_HOT_PIXEL_MAP_MODE,CaptureRequest.COLOR_CORRECTION_MODE,
        CaptureRequest.COLOR_CORRECTION_GAINS,CaptureRequest.COLOR_CORRECTION_TRANSFORM));
    private static final List<CaptureResult.Key<?>> STANDARD_RESULTS=Collections.unmodifiableList(Arrays.asList(
        (CaptureResult.Key<?>)CaptureResult.CONTROL_ZOOM_RATIO,CaptureResult.LOGICAL_MULTI_CAMERA_ACTIVE_PHYSICAL_ID,
        CaptureResult.TONEMAP_MODE,CaptureResult.TONEMAP_CURVE,SHARPNESS_RESULT,
        CaptureResult.COLOR_CORRECTION_MODE,CaptureResult.COLOR_CORRECTION_GAINS,
        CaptureResult.COLOR_CORRECTION_TRANSFORM,CaptureResult.STATISTICS_HOT_PIXEL_MAP_MODE,
        CaptureResult.STATISTICS_HOT_PIXEL_MAP,CaptureResult.HOT_PIXEL_MODE));
    private static final List<CaptureResult.Key<?>> SURFACE_RESULTS=Collections.unmodifiableList(Arrays.asList(
        NATIVE_TRACE,FACE_ATTRIBUTES,ESTIMATED_SNAPSHOT_EXP_TIME,HOT_PIXEL_MAP_AVAILABLE,APPLIED_NATIVE_MODE,
        VIDEO_SCENE_TIMELINE,STAGE_RECORDS,HARDWARE_LEVEL_ADVERTISED,AR_POSE,AR_POSE_CONFIDENCE));
    /** Request controls that are provider-owned: consumed here, never submitted
     * to the HAL (the postprocess section is provider-only, contract 3.2). */
    private static final List<CaptureRequest.Key<?>> PROVIDER_REQUESTS=Collections.unmodifiableList(Arrays.asList(
        WATERMARK_LAYOUT,MIRROR,EXIF_USER_COMMENT));
    /** Extra modes an application may request through {@code stillMode}. */
    private static final int[] SELECTABLE_STILL_MODES={NATIVE_OFF,NATIVE_FILTER,NATIVE_SINGLE_BOKEH,
        NATIVE_PICBEST,NATIVE_IMAGE_ADJUST,NATIVE_SINGLE_RELIGHT,NATIVE_DOC,NATIVE_SCES,
        NATIVE_DOUBLE_EXPOSURE,NATIVE_LONG_EXPOSURE,NATIVE_AR_TRACK,NATIVE_REMOSAIC};

    /** Request controls an application may set for this camera and extension
     * mode. This is the answer to "which parameter is available here"; the
     * Extensions framework exposes the same list through
     * {@code CameraExtensionCharacteristics.getAvailableCaptureRequestKeys()}. */
    public static List<CaptureRequest.Key<?>> availableRequests(CameraCharacteristics c,int extensionMode) {
        List<CaptureRequest.Key<?>> filtered=new ArrayList<>();
        if(c==null)return filtered;
        List<CaptureRequest.Key<?>> keys=new ArrayList<>(standardRequests(c,extensionMode));
        keys.addAll(vendorRequests(c,extensionMode));
        List<?> advertised=c.getAvailableCaptureRequestKeys();
        for(CaptureRequest.Key<?> key:keys)
            if(isAdvertised(advertised,key))filtered.add(key);
        return filtered;
    }

    /** Result keys this camera and extension mode can return. */
    public static List<CaptureResult.Key<?>> availableResults(CameraCharacteristics c,int extensionMode) {
        List<CaptureResult.Key<?>> filtered=new ArrayList<>();
        if(c==null)return filtered;
        List<CaptureResult.Key<?>> keys=new ArrayList<>(standardResults(c,extensionMode));
        for(CaptureRequest.Key<?> request:vendorRequests(c,extensionMode))
            for(Map.Entry<String,CaptureResult.Key<?>> entry:SNAPSHOT_RESULTS.entrySet())
                if(entry.getKey().equals(request.getName()))keys.add(entry.getValue());
        keys.addAll(transportResults(c,extensionMode));
        List<?> advertised=c.getAvailableCaptureResultKeys();
        for(CaptureResult.Key<?> key:keys)
            if(isAdvertisedResult(advertised,key))filtered.add(key);
        return filtered;
    }

    /** True when {@code key} is a surface parameter usable for this camera and
     * extension mode. Unknown keys return false. */
    public static boolean isAvailable(CameraCharacteristics c,int extensionMode,CaptureRequest.Key<?> key) {
        if(key==null)return false;
        for(CaptureRequest.Key<?> candidate:availableRequests(c,extensionMode))
            if(candidate.getName().equals(key.getName()))return true;
        return false;
    }

    /** Short human-readable bound text for capability UIs and the validation app. */
    public static String describe(CaptureRequest.Key<?> key) {
        if(key==null)return "unknown control";
        String name=key.getName();
        if(name.equals(FRONT_BEAUTY_STRENGTH.getName()))return "int32 0..100";
        if(name.equals(PIC_BEST_FRAMES.getName()))return "int32 2..8";
        if(name.equals(PIC_BEST_STRENGTH.getName()))return "int32 0..100";
        if(name.equals(PIC_BEST_OPEN_EYE_ONLY.getName()))return "int32 0/1";
        if(name.equals(BLURLESS_THRESHOLD.getName()))return "int32 0..1000";
        if(name.equals(FACE_INFO_MODE.getName()))return "int32 0 off / 1 select / 2 select+stabilize";
        if(name.equals(IMAGE_ADJUST.getName()))return "int32[6] brightness,contrast,saturation,hue,rGain,bGain -100..100";
        if(name.equals(SINGLE_BOKEH_STRENGTH.getName()))return "int32 0..100";
        if(name.equals(SINGLE_BOKEH_SPOT_MODE.getName()))return "int32 0 face / 1 center / 2 manual";
        if(name.equals(SINGLE_BOKEH_SPOT.getName()))return "float[2] normalized 0..1";
        if(name.equals(PORTRAIT_LIGHT_MODE.getName()))return "int32 0 off / 1..8 direction / 9 auto";
        if(name.equals(PORTRAIT_LIGHT_STRENGTH.getName()))return "int32 0..100";
        if(name.equals(DOC_MODE.getName()))return "int32 0 off / 1 standard / 2 shadow / 3 text";
        if(name.equals(DOC_STRENGTH.getName()))return "int32 0..100";
        if(name.equals(AUTO_HDR_ENABLE.getName())||name.equals(AUTO_NIGHT_ENABLE.getName()))return "int32 0/1";
        if(name.equals(DOUBLE_EXPOSURE_MODE.getName()))return "int32 0 off / 1 manual / 2 auto";
        if(name.equals(DOUBLE_EXPOSURE_STRENGTH.getName()))return "int32 0..100";
        if(name.equals(DOUBLE_EXPOSURE_TIME_NS.getName()))return "int64 advertised long-exposure range";
        if(name.equals(LONG_EXPOSURE_ENABLE.getName())||name.equals(REMOSAIC_ENABLE.getName()))return "int32 0/1";
        if(name.equals(EXIF_COMMENT.getName())||name.equals(EXIF_USER_COMMENT.getName()))return "byte UTF-8 <=256";
        if(name.equals(WATERMARK_ASSET.getName())||name.equals(STICKER_ASSET.getName()))return "byte UTF-8 1..256";
        if(name.equals(STICKER_RECT.getName()))return "float[4] normalized l,t,r,b";
        if(name.equals(WATERMARK_LAYOUT.getName()))return "int32[4] ten-thousandths l,t,r,b";
        if(name.equals(MIRROR_PREVIEW.getName())||name.equals(MIRROR.getName()))return "int32 0/1";
        if(name.equals(APP_POLICY_MODE.getName()))return "int32 0 standard / 1 policy table";
        if(name.equals(APP_MASK.getName()))return "int32 bitmask 1 beauty,2 filter,4 scene,8 watermark,16 AR";
        if(name.equals(FACE_PREVIEW_PARAM.getName()))return "int32[4] mode 0..2,minLevel 0..100,maxFaces 1..10,hint 0/1";
        if(name.equals(DEPTH_QUALITY.getName()))return "int32 0..3";
        if(name.equals(STILL_MODE.getName()))return "int32 advertised availableModes";
        if(name.equals(FILTER_TYPE.getName()))return "int32 0 or an advertised availableFilterTypes value";
        if(name.equals(FILTER_FLIP_MODE.getName()))return "int32 0 only";
        if(name.equals(LOW_POWER_MODE.getName())||name.equals(DEPTH_STREAM_ENABLED.getName()))return "int32 0/1";
        if(name.equals(RELIGHT_STRENGTH.getName()))return "int32 0..100";
        if(name.equals(RELIGHT_AZIMUTH.getName()))return "int32 -180..180 degrees";
        if(name.equals(RELIGHT_ELEVATION.getName()))return "int32 0..90 degrees";
        if(name.equals(SLOW_MOTION_RATIO.getName()))return "int32 0 off or an advertised ratio";
        if(name.equals(VIDEO_HDR_PROFILE.getName()))return "int32 0 SDR / 1 HLG10";
        if(name.equals(LOGICAL_ZOOM_HINT.getName()))return "float inside the advertised zoom range";
        if(name.equals(IR_STREAM_MODE.getName()))return "int32 0/1/2";
        if(name.equals(AR_TRACKING_ENABLE.getName()))return "int32 0/1";
        if(name.equals(AR_TRACKING_MODE.getName()))return "int32 0 orientation / 1 orientation+translation";
        return "standard camera2 control";
    }

    /** Validates one application-supplied parameter. Keys that are neither a
     * surface control nor an advertised standard surface key keep the existing
     * framework-filtered behaviour. */
    static void validate(CameraCharacteristics c,int extensionMode,List<?> advertised,
            CaptureRequest.Key<?> key,Object value) {
        if(key==null)return;
        CaptureRequest.Key<?> known=byName(key.getName());
        if(known==null)return;
        if(!isAdvertised(advertised,known)) {
            // A round-2 parameter is rejected when this camera or mode cannot
            // provide it. An unadvertised standard key keeps the framework
            // filter (the platform explicitly allows disregarding it).
            if(!surface(known))return;
            throw new IllegalArgumentException("Parameter "+key.getName()+" is not available for this camera and extension ("
                +describe(known)+")");
        }
        if(value==null)return;
        bounds(c,extensionMode,advertised,known,value);
    }

    /** Cross-parameter rules that a single value cannot express. Called after a
     * batch has been merged so a rejected batch never mutates any control. */
    static void validateBatch(CameraCharacteristics c,int extensionMode,
            List<?> advertised,Map<CaptureRequest.Key<?>,Object> controls,Size outputSize) {
        Object longEnable=find(controls,LONG_EXPOSURE_ENABLE);
        Object exposure=find(controls,CaptureRequest.SENSOR_EXPOSURE_TIME);
        if(integer(longEnable,0)!=0) {
            requireAvailable(advertised,LONG_EXPOSURE_ENABLE,"long exposure");
            if(exposure==null)
                throw new IllegalArgumentException("longExposureEnable=1 requires standard SENSOR_EXPOSURE_TIME");
            manualAdmission(c,advertised,"long exposure");
            confirmExposureMode(controls);
            long value=longValue(exposure);
            long[] range=longRange(c);
            if(range!=null&&(value<range[0]||value>range[1]))
                throw new IllegalArgumentException("SENSOR_EXPOSURE_TIME "+value+" ns is outside the advertised longExposureRangeNs ["
                    +range[0]+", "+range[1]+"]");
            long maximum=maximumLongExposure(c);
            if(maximum>0&&value>maximum)
                throw new IllegalArgumentException("SENSOR_EXPOSURE_TIME "+value+" ns exceeds maxLongExposureNs "+maximum);
        } else if(exposure!=null) {
            long maximum=maximumLongExposure(c);
            if(maximum>0&&longValue(exposure)>maximum)
                throw new IllegalArgumentException("SENSOR_EXPOSURE_TIME exceeds maxLongExposureNs "+maximum
                    +"; set longExposureEnable=1 to admit it");
        }
        Object doubleMode=find(controls,DOUBLE_EXPOSURE_MODE);
        int mode=integer(doubleMode,0);
        if(mode!=0) {
            requireAvailable(advertised,DOUBLE_EXPOSURE_MODE,"double exposure");
            manualAdmission(c,advertised,"double exposure");
            Object second=find(controls,DOUBLE_EXPOSURE_TIME_NS);
            if(mode==1) {
                if(second==null)
                    throw new IllegalArgumentException("doubleExposureMode=1 requires DOUBLE_EXPOSURE_TIME_NS");
                confirmExposureMode(controls);
            }
            if(second!=null) {
                long value=longValue(second);
                long maximum=maximumLongExposure(c);
                long[] range=longRange(c);
                if(value<=0)throw new IllegalArgumentException("doubleExposureTimeNs must be positive");
                if(maximum>0&&value>maximum)
                    throw new IllegalArgumentException("doubleExposureTimeNs exceeds maxLongExposureNs "+maximum);
                if(range!=null&&(value<range[0]||value>range[1]))
                    throw new IllegalArgumentException("doubleExposureTimeNs is outside the advertised longExposureRangeNs");
            }
        }
        Object spotMode=find(controls,SINGLE_BOKEH_SPOT_MODE);
        Object spot=find(controls,SINGLE_BOKEH_SPOT);
        if(integer(spotMode,0)==2&&spot==null)
            throw new IllegalArgumentException("singleBokehSpotMode=2 requires SINGLE_BOKEH_SPOT");
        if(spot!=null&&integer(spotMode,0)!=2)
            throw new IllegalArgumentException("SINGLE_BOKEH_SPOT requires singleBokehSpotMode=2");
        Object remosaic=find(controls,REMOSAIC_ENABLE);
        if(integer(remosaic,0)!=0&&outputSize!=null) {
            int[] sizes=get(c,AVAILABLE_REMOSAIC_SIZES);
            if(sizes!=null&&sizes.length>0&&!containsSize(sizes,outputSize))
                throw new IllegalArgumentException("remosaicEnable requires an advertised remosaic size ("
                    +outputSize.getWidth()+"x"+outputSize.getHeight()+" is not in availableRemosaicSizes)");
        }
        Object doc=find(controls,DOC_MODE);
        if(integer(doc,0)!=0) {
            Object sceneMode=find(controls,CaptureRequest.CONTROL_SCENE_MODE);
            if(sceneMode!=null&&integer(sceneMode,0)!=0&&
                    integer(sceneMode,0)!=CaptureRequest.CONTROL_SCENE_MODE_BARCODE)
                throw new IllegalArgumentException("docMode requires the standard barcode scene mode or no explicit scene mode");
        }
        Object adjust=find(controls,IMAGE_ADJUST);
        if(adjust instanceof int[]&&nonzero((int[])adjust)) {
            int[] values=(int[])adjust;
            if(adjustToneRequested(values)&&!isAdvertised(advertised,CaptureRequest.TONEMAP_CURVE))
                throw new IllegalArgumentException("imageAdjust brightness/contrast requires standard TONEMAP_CURVE support");
            if(adjustColorRequested(values)&&!isAdvertised(advertised,CaptureRequest.COLOR_CORRECTION_TRANSFORM))
                throw new IllegalArgumentException("imageAdjust saturation/hue requires standard COLOR_CORRECTION_TRANSFORM support");
            if(adjustGainRequested(values)&&!isAdvertised(advertised,CaptureRequest.COLOR_CORRECTION_GAINS))
                throw new IllegalArgumentException("imageAdjust rGain/bGain requires standard COLOR_CORRECTION_GAINS support");
            Object toneMode=find(controls,CaptureRequest.TONEMAP_MODE);
            if(adjustToneRequested(values)&&toneMode!=null&&
                    integer(toneMode,-1)!=CaptureRequest.TONEMAP_MODE_CONTRAST_CURVE)
                throw new IllegalArgumentException("imageAdjust requires TONEMAP_MODE_CONTRAST_CURVE");
            Object colorMode=find(controls,CaptureRequest.COLOR_CORRECTION_MODE);
            if((adjustColorRequested(values)||adjustGainRequested(values))&&colorMode!=null&&
                    integer(colorMode,-1)!=CaptureRequest.COLOR_CORRECTION_MODE_TRANSFORM_MATRIX)
                throw new IllegalArgumentException("imageAdjust requires COLOR_CORRECTION_MODE_TRANSFORM_MATRIX");
        }
    }

    /** Standard Camera2 keys submitted for the surface parameters, plus the
     * manual-exposure gate. Explicit application keys win over derived keys,
     * except for the exposure mode the manual route owns. */
    static Map<CaptureRequest.Key<?>,Object> standard(CameraCharacteristics c,int extensionMode,
            Map<CaptureRequest.Key<?>,Object> controls) {
        Map<CaptureRequest.Key<?>,Object> values=new LinkedHashMap<>();
        int[] adjust=adjustment(controls);
        if(adjust!=null) {
            if(adjustToneRequested(adjust)&&isAdvertised(c,CaptureRequest.TONEMAP_CURVE)&&
                    isAdvertised(c,CaptureRequest.TONEMAP_MODE)) {
                values.put(CaptureRequest.TONEMAP_MODE,CaptureRequest.TONEMAP_MODE_CONTRAST_CURVE);
                values.put(CaptureRequest.TONEMAP_CURVE,tonemapCurve(adjust[0],adjust[1]));
            }
            boolean color=adjustColorRequested(adjust)&&isAdvertised(c,CaptureRequest.COLOR_CORRECTION_TRANSFORM);
            boolean gains=adjustGainRequested(adjust)&&isAdvertised(c,CaptureRequest.COLOR_CORRECTION_GAINS);
            if(color||gains) {
                values.put(CaptureRequest.COLOR_CORRECTION_MODE,CaptureRequest.COLOR_CORRECTION_MODE_TRANSFORM_MATRIX);
                if(color)values.put(CaptureRequest.COLOR_CORRECTION_TRANSFORM,colorMatrix(adjust[2],adjust[3]));
                if(gains)values.put(CaptureRequest.COLOR_CORRECTION_GAINS,whiteBalanceGains(adjust[4],adjust[5]));
            }
        }
        if(manualExposure(controls)) {
            values.put(CaptureRequest.CONTROL_AE_MODE,CaptureRequest.CONTROL_AE_MODE_OFF);
            long exposure=longValue(find(controls,CaptureRequest.SENSOR_EXPOSURE_TIME));
            long second=longValue(find(controls,DOUBLE_EXPOSURE_TIME_NS));
            long needed=Math.max(exposure,0)+Math.max(second,0);
            Object current=find(controls,CaptureRequest.SENSOR_FRAME_DURATION);
            if(needed>0&&(current==null||longValue(current)<needed))
                values.put(CaptureRequest.SENSOR_FRAME_DURATION,needed);
        }
        return values;
    }

    /** Scene-mode mapping for DOC (contract section 2 mode 15). Applied after the
     * native capture block so the documented standard surface wins. */
    static Map<CaptureRequest.Key<?>,Object> scene(CameraCharacteristics c,int extensionMode,
            Map<CaptureRequest.Key<?>,Object> controls) {
        Map<CaptureRequest.Key<?>,Object> values=new LinkedHashMap<>();
        if(extensionMode!=MODE_AUTO)return values;
        if(integer(find(controls,DOC_MODE),0)==0)return values;
        // An explicitly selected native DOC stage (mode 15) owns scene
        // compatibility in the HAL's own admission check, so the provider does
        // not combine it with a second scene request. The standard barcode
        // scene mode is used whenever the native stage is not selected.
        if(integer(find(controls,STILL_MODE),0)==NATIVE_DOC)return values;
        if(!isAdvertised(c,CaptureRequest.CONTROL_SCENE_MODE))return values;
        if(!contains(c.get(CameraCharacteristics.CONTROL_AVAILABLE_SCENE_MODES),CaptureRequest.CONTROL_SCENE_MODE_BARCODE))
            return values;
        values.put(CaptureRequest.CONTROL_MODE,CaptureRequest.CONTROL_MODE_USE_SCENE_MODE);
        values.put(CaptureRequest.CONTROL_SCENE_MODE,CaptureRequest.CONTROL_SCENE_MODE_BARCODE);
        return values;
    }

    /** HAL request template: the manual route (long/double exposure) needs the
     * standard MANUAL template; everything else keeps today's behaviour. */
    static int template(boolean still,Map<CaptureRequest.Key<?>,Object> controls) {
        if(still&&manualExposure(controls))return android.hardware.camera2.CameraDevice.TEMPLATE_MANUAL;
        return still?android.hardware.camera2.CameraDevice.TEMPLATE_STILL_CAPTURE:
            android.hardware.camera2.CameraDevice.TEMPLATE_PREVIEW;
    }

    /** Requested native still mode through the existing {@code stillMode}
     * transport, or 0 when the application did not select one. */
    static int requestedStillMode(Map<CaptureRequest.Key<?>,Object> controls) {
        return integer(find(controls,STILL_MODE),0);
    }

    /** The native still mode an explicit application selection permits. */
    static void validateStillMode(CameraCharacteristics c,int extensionMode,
            List<?> advertised,int mode) {
        if(mode==0)return;
        if(mode<0||mode>NATIVE_REMOSAIC)
            throw new IllegalArgumentException("stillMode must be between 0 and "+NATIVE_REMOSAIC+" (got "+mode+")");
        if(!contains(SELECTABLE_STILL_MODES,mode))
            throw new IllegalArgumentException("stillMode "+mode+" is selected by its own extension type, not by stillMode");
        if(!contains(nativeModes(c),mode))
            throw new IllegalArgumentException("stillMode "+mode+" is not advertised in availableModes");
        if(!isAdvertised(advertised,STILL_MODE))
            throw new IllegalArgumentException("stillMode is not available for this camera");
        if(extensionMode==MODE_HDR||extensionMode==MODE_NIGHT)
            throw new IllegalArgumentException("HDR/NIGHT own their native still mode");
    }

    /** Copies the submitted snapshot of every advertised surface result key that
     * the HAL did not itself return. */
    @SuppressWarnings("unchecked")
    static void snapshotResults(Map<CaptureResult.Key,Object> values,
            List<?> advertised,Map<CaptureRequest.Key<?>,Object> controls) {
        for(Map.Entry<String,CaptureResult.Key<?>> entry:SNAPSHOT_RESULTS.entrySet()) {
            CaptureRequest.Key<?> request=byName(entry.getKey());
            if(request==null)continue;
            Object submitted=find(controls,request);
            if(submitted==null)continue;
            CaptureResult.Key<?> result=entry.getValue();
            if(!isAdvertisedResult(advertised,result))continue;
            boolean present=false;
            for(CaptureResult.Key<?> candidate:values.keySet())
                if(candidate!=null&&candidate.getName().equals(result.getName()))present=true;
            if(!present)values.put((CaptureResult.Key)result,copy(submitted));
        }
    }

    /** UTF-8 EXIF UserComment requested through either provider or HAL control. */
    static byte[] jpegComment(Map<CaptureRequest.Key<?>,Object> controls) {
        Object comment=find(controls,EXIF_USER_COMMENT);
        if(comment instanceof byte[])return (byte[])comment;
        Object hal=find(controls,EXIF_COMMENT);
        return hal instanceof byte[]?(byte[])hal:null;
    }

    /** True when the key is a round-2 parameter of this surface. */
    static boolean surface(CaptureRequest.Key<?> key) {
        if(key==null)return false;
        for(CaptureRequest.Key<?> candidate:ALL_REQUESTS)
            if(candidate.getName().equals(key.getName()))return true;
        return false;
    }

    /** True when the key is one of the standard Camera2 surface controls. */
    static boolean standard(CaptureRequest.Key<?> key) {
        if(key==null)return false;
        for(CaptureRequest.Key<?> candidate:STANDARD_SURFACE)
            if(candidate.getName().equals(key.getName()))return true;
        return false;
    }

    /** True when the result key belongs to the standard Camera2 surface. */
    static boolean standardResult(CaptureResult.Key<?> key) {
        if(key==null)return false;
        for(CaptureResult.Key<?> candidate:STANDARD_RESULTS)
            if(candidate.getName().equals(key.getName()))return true;
        return false;
    }

    /** Removes every provider-owned control from a HAL request snapshot. */
    static void stripProvider(Map<CaptureRequest.Key<?>,Object> values) {
        values.keySet().removeIf(OppoSurfaceControls::providerOwned);
    }

    static boolean providerOwned(CaptureRequest.Key<?> key) {
        if(key==null)return false;
        for(CaptureRequest.Key<?> candidate:PROVIDER_REQUESTS)
            if(candidate.getName().equals(key.getName()))return true;
        return false;
    }

    /** Name-based lookup: the framework transports typed keys whose instances
     * are not the ones created here. */
    static Object find(Map<CaptureRequest.Key<?>,Object> controls,CaptureRequest.Key<?> key) {
        if(controls==null||key==null)return null;
        Object direct=controls.get(key);
        if(direct!=null)return direct;
        for(Map.Entry<CaptureRequest.Key<?>,Object> entry:controls.entrySet())
            if(entry.getKey()!=null&&key.getName().equals(entry.getKey().getName()))return entry.getValue();
        return null;
    }

    static CaptureRequest.Key<?> byName(String name) {
        if(name==null)return null;
        for(CaptureRequest.Key<?> key:ALL_REQUESTS)if(key.getName().equals(name))return key;
        for(CaptureRequest.Key<?> key:STANDARD_SURFACE)if(key.getName().equals(name))return key;
        return null;
    }

    static boolean isAdvertised(List<?> advertised,CaptureRequest.Key<?> key) {
        if(advertised==null||key==null)return false;
        for(Object candidate:advertised)
            if(candidate instanceof CaptureRequest.Key&&
                    key.getName().equals(((CaptureRequest.Key<?>)candidate).getName()))return true;
        return false;
    }

    static boolean isAdvertisedResult(List<?> advertised,CaptureResult.Key<?> key) {
        if(advertised==null||key==null)return false;
        for(Object candidate:advertised)
            if(candidate instanceof CaptureResult.Key&&
                    key.getName().equals(((CaptureResult.Key<?>)candidate).getName()))return true;
        return false;
    }

    private static final Map<String,CaptureResult.Key<?>> SNAPSHOT_RESULTS;
    static {
        Map<String,CaptureResult.Key<?>> results=new LinkedHashMap<>();
        KeyPair[] pairs={
            new KeyPair(FRONT_BEAUTY_STRENGTH,Integer.class),new KeyPair(PIC_BEST_FRAMES,Integer.class),
            new KeyPair(PIC_BEST_STRENGTH,Integer.class),new KeyPair(PIC_BEST_OPEN_EYE_ONLY,Integer.class),
            new KeyPair(BLURLESS_THRESHOLD,Integer.class),new KeyPair(FACE_INFO_MODE,Integer.class),
            new KeyPair(IMAGE_ADJUST,int[].class),new KeyPair(SINGLE_BOKEH_STRENGTH,Integer.class),
            new KeyPair(SINGLE_BOKEH_SPOT_MODE,Integer.class),new KeyPair(SINGLE_BOKEH_SPOT,float[].class),
            new KeyPair(PORTRAIT_LIGHT_MODE,Integer.class),new KeyPair(PORTRAIT_LIGHT_STRENGTH,Integer.class),
            new KeyPair(DOC_MODE,Integer.class),new KeyPair(DOC_STRENGTH,Integer.class),
            new KeyPair(AUTO_HDR_ENABLE,Integer.class),new KeyPair(AUTO_NIGHT_ENABLE,Integer.class),
            new KeyPair(DOUBLE_EXPOSURE_MODE,Integer.class),new KeyPair(DOUBLE_EXPOSURE_STRENGTH,Integer.class),
            new KeyPair(DOUBLE_EXPOSURE_TIME_NS,Long.class),new KeyPair(LONG_EXPOSURE_ENABLE,Integer.class),
            new KeyPair(REMOSAIC_ENABLE,Integer.class),new KeyPair(EXIF_COMMENT,byte[].class),
            new KeyPair(WATERMARK_ASSET,byte[].class),new KeyPair(STICKER_ASSET,byte[].class),
            new KeyPair(STICKER_RECT,float[].class),new KeyPair(MIRROR_PREVIEW,Integer.class),
            new KeyPair(APP_POLICY_MODE,Integer.class),new KeyPair(APP_MASK,Integer.class),
            new KeyPair(FACE_PREVIEW_PARAM,int[].class),new KeyPair(DEPTH_QUALITY,Integer.class),
            new KeyPair(STILL_MODE,Integer.class),new KeyPair(FILTER_TYPE,Integer.class),
            new KeyPair(FILTER_FLIP_MODE,Integer.class),new KeyPair(RELIGHT_STRENGTH,Integer.class),
            new KeyPair(RELIGHT_AZIMUTH,Integer.class),new KeyPair(RELIGHT_ELEVATION,Integer.class),
            new KeyPair(SLOW_MOTION_RATIO,Integer.class),new KeyPair(VIDEO_HDR_PROFILE,Integer.class),
            new KeyPair(LOGICAL_ZOOM_HINT,Float.class),new KeyPair(LOW_POWER_MODE,Integer.class),
            new KeyPair(DEPTH_STREAM_ENABLED,Integer.class),
            new KeyPair(IR_STREAM_MODE,Integer.class),new KeyPair(AR_TRACKING_ENABLE,Integer.class),
            new KeyPair(AR_TRACKING_MODE,Integer.class),new KeyPair(WATERMARK_LAYOUT,int[].class),
            new KeyPair(MIRROR,Integer.class),new KeyPair(EXIF_USER_COMMENT,byte[].class)};
        for(KeyPair pair:pairs)
            results.put(pair.key.getName(),new CaptureResult.Key<>(pair.key.getName(),pair.type));
        SNAPSHOT_RESULTS=Collections.unmodifiableMap(results);
    }
    private static final class KeyPair {
        final CaptureRequest.Key<?> key;final Class<?> type;
        KeyPair(CaptureRequest.Key<?> k,Class<?> t){key=k;type=t;}
    }

    // ---- standard surface ----
    private static List<CaptureRequest.Key<?>> standardRequests(CameraCharacteristics c,int mode) {
        List<CaptureRequest.Key<?>> keys=new ArrayList<>();
        if(c==null)return keys;
        if(isAdvertised(c,CaptureRequest.CONTROL_ZOOM_RATIO))keys.add(CaptureRequest.CONTROL_ZOOM_RATIO);
        if(processed(mode)) {
            if(manualSensor(c)) {
                keys.add(CaptureRequest.SENSOR_EXPOSURE_TIME);keys.add(CaptureRequest.SENSOR_SENSITIVITY);
                keys.add(CaptureRequest.SENSOR_FRAME_DURATION);
            }
            if(toneMapSupported(c)){keys.add(CaptureRequest.TONEMAP_MODE);keys.add(CaptureRequest.TONEMAP_CURVE);}
            if(isAdvertised(c,SHARPNESS))keys.add(SHARPNESS);
            if(colorCorrectionSupported(c))keys.add(CaptureRequest.COLOR_CORRECTION_MODE);
            if(awbOff(c)) {
                if(isAdvertised(c,CaptureRequest.COLOR_CORRECTION_GAINS))keys.add(CaptureRequest.COLOR_CORRECTION_GAINS);
                if(isAdvertised(c,CaptureRequest.COLOR_CORRECTION_TRANSFORM))keys.add(CaptureRequest.COLOR_CORRECTION_TRANSFORM);
            }
        }
        if(hotPixelMapSupported(c))keys.add(CaptureRequest.STATISTICS_HOT_PIXEL_MAP_MODE);
        return keys;
    }

    private static List<CaptureResult.Key<?>> standardResults(CameraCharacteristics c,int mode) {
        List<CaptureResult.Key<?>> keys=new ArrayList<>();
        if(c==null)return keys;
        keys.add(CaptureResult.CONTROL_ZOOM_RATIO);
        keys.add(CaptureResult.LOGICAL_MULTI_CAMERA_ACTIVE_PHYSICAL_ID);
        if(processed(mode))keys.addAll(STANDARD_RESULTS);
        return keys;
    }

    // ---- vendor surface ----
    private static List<CaptureRequest.Key<?>> vendorRequests(CameraCharacteristics c,int mode) {
        List<CaptureRequest.Key<?>> keys=new ArrayList<>();
        if(c==null)return keys;
        if(processed(mode)) {
            if(mode==MODE_RETOUCH&&nativeMode(c,NATIVE_FRONT_BEAUTY))keys.add(FRONT_BEAUTY_STRENGTH);
            if((mode==MODE_AUTO||mode==MODE_RETOUCH)&&nativeMode(c,NATIVE_PICBEST)) {
                keys.add(PIC_BEST_FRAMES);keys.add(PIC_BEST_STRENGTH);keys.add(BLURLESS_THRESHOLD);
                keys.add(FACE_INFO_MODE);
                if(fullFaces(c))keys.add(PIC_BEST_OPEN_EYE_ONLY);
            }
            if(imageAdjustSupported(c))keys.add(IMAGE_ADJUST);
            if(mode==MODE_BOKEH&&nativeMode(c,NATIVE_SINGLE_BOKEH)) {
                keys.add(SINGLE_BOKEH_STRENGTH);keys.add(SINGLE_BOKEH_SPOT_MODE);keys.add(SINGLE_BOKEH_SPOT);
            }
            if(mode==MODE_RETOUCH&&nativeMode(c,NATIVE_SINGLE_RELIGHT)) {
                keys.add(PORTRAIT_LIGHT_MODE);keys.add(PORTRAIT_LIGHT_STRENGTH);
            }
            if(mode==MODE_BOKEH&&nativeMode(c,NATIVE_DUAL_RELIGHT)) {
                keys.add(PORTRAIT_LIGHT_MODE);keys.add(PORTRAIT_LIGHT_STRENGTH);
                keys.add(RELIGHT_STRENGTH);keys.add(RELIGHT_AZIMUTH);keys.add(RELIGHT_ELEVATION);
            }
            if(mode==MODE_AUTO&&nativeMode(c,NATIVE_DOC)){keys.add(DOC_MODE);keys.add(DOC_STRENGTH);}
            if(mode==MODE_AUTO) {
                if(nativeMode(c,NATIVE_HDR))keys.add(AUTO_HDR_ENABLE);
                if(nativeMode(c,NATIVE_LLHDR))keys.add(AUTO_NIGHT_ENABLE);
                if(nativeMode(c,NATIVE_DOUBLE_EXPOSURE)&&manualAdmission(c))keys.addAll(Arrays.asList(
                    DOUBLE_EXPOSURE_MODE,DOUBLE_EXPOSURE_STRENGTH,DOUBLE_EXPOSURE_TIME_NS));
                if(manualAdmission(c)&&longExposureAdvertised(c))keys.add(LONG_EXPOSURE_ENABLE);
                if(flag(c,REMOSAIC_SUPPORTED)&&nonempty(get(c,AVAILABLE_REMOSAIC_SIZES)))keys.add(REMOSAIC_ENABLE);
                if(arTrackingSupported(c)){keys.add(AR_TRACKING_ENABLE);keys.add(AR_TRACKING_MODE);}
                if(flag(c,DEPTH_SUPPORTED)){keys.add(DEPTH_QUALITY);keys.add(DEPTH_STREAM_ENABLED);}
                if(nonempty(get(c,AVAILABLE_SLOW_MOTION_RATIOS)))keys.add(SLOW_MOTION_RATIO);
                if(flag(c,HLG_VIDEO_SUPPORTED)&&hlgProfile(c))keys.add(VIDEO_HDR_PROFILE);
            }
            if(mode==MODE_AUTO||mode==MODE_RETOUCH) {
                keys.add(EXIF_COMMENT);keys.add(WATERMARK_ASSET);keys.add(STICKER_ASSET);keys.add(STICKER_RECT);
                keys.add(WATERMARK_LAYOUT);keys.add(MIRROR);keys.add(EXIF_USER_COMMENT);
            }
            if(fullFaces(c)&&mode==MODE_RETOUCH)keys.add(FACE_PREVIEW_PARAM);
            keys.add(APP_POLICY_MODE);keys.add(APP_MASK);
            keys.add(MIRROR_PREVIEW);keys.add(LOW_POWER_MODE);keys.add(LOGICAL_ZOOM_HINT);
            if(nativeMode(c,NATIVE_FILTER)){keys.add(FILTER_TYPE);keys.add(FILTER_FLIP_MODE);}
            keys.add(IR_STREAM_MODE);
            if(nativeModes(c).length>0)keys.add(STILL_MODE);
        }
        return keys;
    }

    private static List<CaptureResult.Key<?>> transportResults(CameraCharacteristics c,int mode) {
        List<CaptureResult.Key<?>> keys=new ArrayList<>();
        if(c==null)return keys;
        keys.addAll(SURFACE_RESULTS);
        if(!arTrackingSupported(c))keys.remove(AR_POSE);
        if(!arTrackingSupported(c))keys.remove(AR_POSE_CONFIDENCE);
        return keys;
    }

    // ---- capability helpers ----
    private static boolean processed(int mode) { return mode==MODE_AUTO||mode==MODE_RETOUCH||mode==MODE_BOKEH; }
    private static int[] nativeModes(CameraCharacteristics c) {
        int[] modes=get(c,OppoAdvancedExtender.NATIVE_MODES);return modes==null?new int[0]:modes;
    }
    private static boolean nativeMode(CameraCharacteristics c,int id){return contains(nativeModes(c),id);}
    private static boolean flag(CameraCharacteristics c,CameraCharacteristics.Key<Byte> key) {
        Byte value=get(c,key);return value!=null&&value!=0;
    }
    private static boolean fullFaces(CameraCharacteristics c) {
        return contains(c.get(CameraCharacteristics.STATISTICS_INFO_AVAILABLE_FACE_DETECT_MODES),
            CaptureRequest.STATISTICS_FACE_DETECT_MODE_FULL);
    }
    private static boolean awbOff(CameraCharacteristics c) {
        return contains(c.get(CameraCharacteristics.CONTROL_AWB_AVAILABLE_MODES),CaptureRequest.CONTROL_AWB_MODE_OFF);
    }
    private static boolean toneMapSupported(CameraCharacteristics c) {
        return isAdvertised(c,CaptureRequest.TONEMAP_CURVE)&&isAdvertised(c,CaptureRequest.TONEMAP_MODE)&&
            contains(c.get(CameraCharacteristics.TONEMAP_AVAILABLE_TONE_MAP_MODES),CaptureRequest.TONEMAP_MODE_CONTRAST_CURVE);
    }
    private static boolean colorCorrectionSupported(CameraCharacteristics c) {
        return awbOff(c)&&(isAdvertised(c,CaptureRequest.COLOR_CORRECTION_TRANSFORM)||
            isAdvertised(c,CaptureRequest.COLOR_CORRECTION_GAINS));
    }
    private static boolean imageAdjustSupported(CameraCharacteristics c) {
        return toneMapSupported(c)||colorCorrectionSupported(c);
    }
    private static boolean hotPixelMapSupported(CameraCharacteristics c) {
        boolean[] modes=c.get(CameraCharacteristics.STATISTICS_INFO_AVAILABLE_HOT_PIXEL_MAP_MODES);
        return isAdvertised(c,CaptureRequest.STATISTICS_HOT_PIXEL_MAP_MODE)&&modes!=null&&contains(modes,true);
    }
    private static boolean manualSensor(CameraCharacteristics c) {
        return contains(c.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES),
            CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR)&&
            contains(c.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_MODES),CaptureRequest.CONTROL_AE_MODE_OFF);
    }
    private static boolean manualAdmission(CameraCharacteristics c) {
        return manualSensor(c)&&isAdvertised(c,CaptureRequest.SENSOR_EXPOSURE_TIME);
    }
    private static boolean longExposureAdvertised(CameraCharacteristics c) {
        return maximumLongExposure(c)>0||longRange(c)!=null;
    }
    private static long maximumLongExposure(CameraCharacteristics c) {
        Long value=get(c,MAX_LONG_EXPOSURE_NS);return value==null?0:value;
    }
    private static long[] longRange(CameraCharacteristics c) {
        long[] value=get(c,LONG_EXPOSURE_RANGE_NS);
        return value!=null&&value.length==2?value:null;
    }
    private static boolean arTrackingSupported(CameraCharacteristics c) {
        return flag(c,AR_TRACKING_SUPPORTED)||flag(c,AR_SECTION_TRACKING_SUPPORTED);
    }
    private static boolean hlgProfile(CameraCharacteristics c) {
        DynamicRangeProfiles profiles=c.get(CameraCharacteristics.REQUEST_AVAILABLE_DYNAMIC_RANGE_PROFILES);
        return profiles!=null&&profiles.getSupportedProfiles().contains(DynamicRangeProfiles.HLG10);
    }
    private static boolean isAdvertised(CameraCharacteristics c,CaptureRequest.Key<?> key) {
        if(c==null)return false;
        List<?> advertised=c.getAvailableCaptureRequestKeys();
        if(advertised==null)return false;
        // A HAL that publishes no key list at all cannot be probed; require the
        // explicit list so an unwired key is rejected rather than dropped.
        return isAdvertised(advertised,key);
    }
    private static boolean manualExposure(Map<CaptureRequest.Key<?>,Object> controls) {
        if(integer(find(controls,LONG_EXPOSURE_ENABLE),0)!=0)return true;
        return integer(find(controls,DOUBLE_EXPOSURE_MODE),0)==1;
    }
    private static void manualAdmission(CameraCharacteristics c,List<?> advertised,String what) {
        if(!manualSensor(c)||(!isAdvertised(advertised,CaptureRequest.SENSOR_EXPOSURE_TIME)&&
                !isAdvertised(c,CaptureRequest.SENSOR_EXPOSURE_TIME)))
            throw new IllegalArgumentException(what+" requires MANUAL_SENSOR, AE_MODE_OFF and standard SENSOR_EXPOSURE_TIME support");
    }
    private static void requireAvailable(List<?> advertised,CaptureRequest.Key<?> key,
            String what) {
        if(!isAdvertised(advertised,key))
            throw new IllegalArgumentException(what+" requires the advertised "+key.getName()+" control");
    }
    private static void confirmExposureMode(Map<CaptureRequest.Key<?>,Object> controls) {
        Object mode=find(controls,CaptureRequest.CONTROL_AE_MODE);
        if(mode!=null&&integer(mode,-1)!=CaptureRequest.CONTROL_AE_MODE_OFF)
            throw new IllegalArgumentException("manual exposure requires CONTROL_AE_MODE_OFF");
    }
    private static int[] adjustment(Map<CaptureRequest.Key<?>,Object> controls) {
        Object value=find(controls,IMAGE_ADJUST);
        return value instanceof int[]&&((int[])value).length==6?(int[])value:null;
    }
    private static boolean nonzero(int[] values) {
        for(int value:values)if(value!=0)return true;return false;
    }
    private static boolean adjustToneRequested(int[] values){return values[0]!=0||values[1]!=0;}
    private static boolean adjustColorRequested(int[] values){return values[2]!=0||values[3]!=0;}
    private static boolean adjustGainRequested(int[] values){return values[4]!=0||values[5]!=0;}
    private static boolean containsSize(int[] sizes,Size size) {
        for(int i=0;i+1<sizes.length;i+=2)
            if(sizes[i]==size.getWidth()&&sizes[i+1]==size.getHeight())return true;
        return false;
    }
    private static boolean nonempty(int[] values){return values!=null&&values.length>0;}
    /** Reads the hidden android.colorCorrection.availableModes list, or null. */
    private static int[] availableColorCorrectionModes(CameraCharacteristics c){
        Object value=c.get(HIDDEN_COLOR_CORRECTION_AVAILABLE_MODES);
        return value instanceof int[]?(int[])value:null;
    }

    // The key is not in the public SDK; the name is the platform one and the
    // lookup simply returns null when the platform does not expose it.
    private static final CameraCharacteristics.Key<int[]> HIDDEN_COLOR_CORRECTION_AVAILABLE_MODES=
            hiddenIntArrayKey("android.colorCorrection.availableModes");

    private static CameraCharacteristics.Key<int[]> hiddenIntArrayKey(String name){
        try{
            return new CameraCharacteristics.Key<>(name,int[].class);
        }catch(RuntimeException e){
            return null;
        }
    }

    private static boolean contains(int[] values,int value) {
        if(values!=null)for(int item:values)if(item==value)return true;
        return false;
    }
    private static boolean contains(boolean[] values,boolean value) {
        if(values!=null)for(boolean item:values)if(item==value)return true;
        return false;
    }
    @SuppressWarnings("unchecked")
    private static <T> T get(CameraCharacteristics c,CameraCharacteristics.Key<T> key) {
        try { return c==null?null:c.get(key); }
        catch(IllegalArgumentException unregistered) { return null; }
    }

    private static int integer(Object value,int fallback) {
        return value instanceof Integer?(Integer)value:fallback;
    }
    private static long longValue(Object value) {
        if(value instanceof Long)return (Long)value;
        if(value instanceof Integer)return (Integer)value;
        return 0;
    }
    private static Object copy(Object value) {
        if(value instanceof int[])return ((int[])value).clone();
        if(value instanceof float[])return ((float[])value).clone();
        if(value instanceof byte[])return ((byte[])value).clone();
        if(value instanceof long[])return ((long[])value).clone();
        return value;
    }

    // ---- bounds ----
    private static void bounds(CameraCharacteristics c,int mode,List<?> advertised,CaptureRequest.Key<?> key,Object value) {
        String name=key.getName();
        if(name.equals(FRONT_BEAUTY_STRENGTH.getName())){range(integer(value,-1),0,100,name);return;}
        if(name.equals(PIC_BEST_FRAMES.getName())){range(integer(value,-1),2,8,name);return;}
        if(name.equals(PIC_BEST_STRENGTH.getName())){range(integer(value,-1),0,100,name);return;}
        if(name.equals(PIC_BEST_OPEN_EYE_ONLY.getName())){range(integer(value,-1),0,1,name);return;}
        if(name.equals(BLURLESS_THRESHOLD.getName())){range(integer(value,-1),0,1000,name);return;}
        if(name.equals(FACE_INFO_MODE.getName())){range(integer(value,-1),0,2,name);return;}
        if(name.equals(IMAGE_ADJUST.getName())) {
            int[] values=intArray(value,6,name);
            for(int component:values)range(component,-100,100,name);
            return;
        }
        if(name.equals(SINGLE_BOKEH_STRENGTH.getName())){range(integer(value,-1),0,100,name);return;}
        if(name.equals(SINGLE_BOKEH_SPOT_MODE.getName())){range(integer(value,-1),0,2,name);return;}
        if(name.equals(SINGLE_BOKEH_SPOT.getName())) {
            float[] spot=floatArray(value,2,name);
            for(float component:spot)finiteRange(component,0f,1f,name);
            return;
        }
        if(name.equals(PORTRAIT_LIGHT_MODE.getName())){range(integer(value,-1),0,9,name);return;}
        if(name.equals(PORTRAIT_LIGHT_STRENGTH.getName())){range(integer(value,-1),0,100,name);return;}
        if(name.equals(DOC_MODE.getName())) {
            int requested=integer(value,-1);range(requested,0,3,name);
            int[] available=get(c,AVAILABLE_DOC_MODES);
            if(available!=null&&available.length>0&&requested!=0&&!contains(available,requested))
                throw new IllegalArgumentException("docMode "+requested+" is not advertised in availableDocModes");
            return;
        }
        if(name.equals(DOC_STRENGTH.getName())){range(integer(value,-1),0,100,name);return;}
        if(name.equals(AUTO_HDR_ENABLE.getName())||name.equals(AUTO_NIGHT_ENABLE.getName())||
                name.equals(LONG_EXPOSURE_ENABLE.getName())||name.equals(REMOSAIC_ENABLE.getName())||
                name.equals(MIRROR_PREVIEW.getName())||name.equals(MIRROR.getName())||
                name.equals(APP_POLICY_MODE.getName())||name.equals(LOW_POWER_MODE.getName())||
                name.equals(DEPTH_STREAM_ENABLED.getName())||name.equals(AR_TRACKING_ENABLE.getName())) {
            range(integer(value,-1),0,1,name);return;
        }
        if(name.equals(DOUBLE_EXPOSURE_MODE.getName())){range(integer(value,-1),0,2,name);return;}
        if(name.equals(DOUBLE_EXPOSURE_STRENGTH.getName())){range(integer(value,-1),0,100,name);return;}
        if(name.equals(DOUBLE_EXPOSURE_TIME_NS.getName())) {
            if(!(value instanceof Long))throw new IllegalArgumentException(name+" expects an int64 value");
            if((Long)value<=0)throw new IllegalArgumentException(name+" must be positive");
            return;
        }
        if(name.equals(EXIF_COMMENT.getName())||name.equals(EXIF_USER_COMMENT.getName())) {
            comment(value,name,true);return;
        }
        if(name.equals(WATERMARK_ASSET.getName())||name.equals(STICKER_ASSET.getName())) {
            comment(value,name,false);return;
        }
        if(name.equals(STICKER_RECT.getName())){rectangle(floatArray(value,4,name),name,true);return;}
        if(name.equals(WATERMARK_LAYOUT.getName())){rectangle(intArray(value,4,name),name,false);return;}
        if(name.equals(APP_MASK.getName())) {
            int mask=integer(value,-1);
            if(mask<0||(mask&~ALL_MASKS)!=0)
                throw new IllegalArgumentException(name+" accepts only bits "+ALL_MASKS+" (beauty,filter,scene,watermark,AR)");
            return;
        }
        if(name.equals(FACE_PREVIEW_PARAM.getName())) {
            int[] values=intArray(value,4,name);
            range(values[0],0,2,name);range(values[1],0,100,name);range(values[2],1,10,name);range(values[3],0,1,name);
            return;
        }
        if(name.equals(DEPTH_QUALITY.getName())){range(integer(value,-1),0,3,name);return;}
        if(name.equals(STILL_MODE.getName())){validateStillMode(c,mode,advertised,integer(value,-1));return;}
        if(name.equals(FILTER_TYPE.getName())) {
            int type=integer(value,-1);
            if(type==0)return;
            if(!contains(get(c,OppoAdvancedExtender.AVAILABLE_FILTER_TYPES),type))
                throw new IllegalArgumentException(name+" "+type+" is not advertised in availableFilterTypes");
            return;
        }
        if(name.equals(FILTER_FLIP_MODE.getName())) {
            if(integer(value,-1)!=0)throw new IllegalArgumentException(name+" currently exposes only 0");
            return;
        }
        if(name.equals(RELIGHT_STRENGTH.getName())){range(integer(value,-1),0,100,name);return;}
        if(name.equals(RELIGHT_AZIMUTH.getName())){range(integer(value,-999),-180,180,name);return;}
        if(name.equals(RELIGHT_ELEVATION.getName())){range(integer(value,-1),0,90,name);return;}
        if(name.equals(SLOW_MOTION_RATIO.getName())) {
            int ratio=integer(value,-1);
            if(ratio==0)return;
            if(!contains(get(c,AVAILABLE_SLOW_MOTION_RATIOS),ratio))
                throw new IllegalArgumentException(name+" "+ratio+" is not advertised in availableSlowMotionRatios");
            return;
        }
        if(name.equals(VIDEO_HDR_PROFILE.getName())) {
            int profile=integer(value,-1);range(profile,0,1,name);
            if(profile==1&&!hlgProfile(c))
                throw new IllegalArgumentException("videoHdrProfile=1 requires standard HLG10 in REQUEST_AVAILABLE_DYNAMIC_RANGE_PROFILES");
            return;
        }
        if(name.equals(LOGICAL_ZOOM_HINT.getName())) {
            if(!(value instanceof Float)||!Float.isFinite((Float)value))
                throw new IllegalArgumentException(name+" expects a finite float");
            float zoom=(Float)value;
            if(zoom<=0f)throw new IllegalArgumentException(name+" must be positive");
            Range<Float> range=c.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE);
            if(range!=null&&!range.contains(zoom))
                throw new IllegalArgumentException(name+" "+zoom+" is outside the advertised zoom range "+range);
            return;
        }
        if(name.equals(IR_STREAM_MODE.getName())){range(integer(value,-1),0,2,name);return;}
        if(name.equals(AR_TRACKING_MODE.getName())){range(integer(value,-1),0,1,name);return;}
        if(name.equals(CaptureRequest.CONTROL_ZOOM_RATIO.getName())) {
            if(!(value instanceof Float)||!Float.isFinite((Float)value))
                throw new IllegalArgumentException("zoom ratio expects a finite float");
            float zoom=(Float)value;
            if(zoom<=0f)throw new IllegalArgumentException("zoom ratio must be positive");
            Range<Float> range=c.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE);
            if(range!=null&&!range.contains(zoom))
                throw new IllegalArgumentException("zoom ratio "+zoom+" is outside the advertised range "+range);
            return;
        }
        if(name.equals(CaptureRequest.TONEMAP_CURVE.getName())) {
            if(!(value instanceof TonemapCurve))throw new IllegalArgumentException("tonemap curve expects a TonemapCurve");
            return;
        }
        if(name.equals(CaptureRequest.TONEMAP_MODE.getName())) {
            int toneMode=integer(value,-1);
            if(!contains(c.get(CameraCharacteristics.TONEMAP_AVAILABLE_TONE_MAP_MODES),toneMode))
                throw new IllegalArgumentException("tonemap mode "+toneMode+" is not advertised by the camera");
            return;
        }
        if(name.equals(SHARPNESS.getName())){range(integer(value,-1),0,3,name);return;}
        if(name.equals(CaptureRequest.STATISTICS_HOT_PIXEL_MAP_MODE.getName())) {
            if(!(value instanceof Boolean))
                throw new IllegalArgumentException(name+" expects a boolean value");
            if(Boolean.TRUE.equals(value)&&!hotPixelMapSupported(c))
                throw new IllegalArgumentException("hot-pixel map mode ON is not advertised by the camera");
            return;
        }
        if(name.equals(CaptureRequest.CONTROL_AE_MODE.getName())) {
            if(!contains(c.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_MODES),integer(value,-1)))
                throw new IllegalArgumentException("AE mode "+value+" is not advertised by the camera");
            return;
        }
        if(name.equals(CaptureRequest.COLOR_CORRECTION_MODE.getName())) {
            int colorMode=integer(value,-1);
            // android.colorCorrection.availableModes is not in the public SDK, so
            // it is read reflectively; an absent key means "not advertised" and
            // the value is accepted (the HAL validates it for real).
            int[] modes=availableColorCorrectionModes(c);
            if(modes!=null&&modes.length>0&&!contains(modes,colorMode))
                throw new IllegalArgumentException("color correction mode "+colorMode+" is not advertised by the camera");
            return;
        }
        if(name.equals(CaptureRequest.COLOR_CORRECTION_GAINS.getName())) {
            if(!(value instanceof RggbChannelVector))
                throw new IllegalArgumentException(name+" expects an RggbChannelVector (standard white-balance gains)");
            return;
        }
        if(name.equals(CaptureRequest.COLOR_CORRECTION_TRANSFORM.getName())) {
            if(!(value instanceof ColorSpaceTransform))
                throw new IllegalArgumentException(name+" expects a ColorSpaceTransform (standard 3x3 matrix)");
            return;
        }
        // Other advertised standard keys keep their platform validation.
    }
    private static void range(int value,int low,int high,String name) {
        if(value<low||value>high)
            throw new IllegalArgumentException(name+" must be between "+low+" and "+high+" (got "+value+")");
    }
    private static void finiteRange(float value,float low,float high,String name) {
        if(!Float.isFinite(value)||value<low||value>high)
            throw new IllegalArgumentException(name+" must be finite and between "+low+" and "+high+" (got "+value+")");
    }
    private static int[] intArray(Object value,int length,String name) {
        if(!(value instanceof int[])||((int[])value).length!=length)
            throw new IllegalArgumentException(name+" expects exactly "+length+" int32 values");
        return (int[])value;
    }
    private static float[] floatArray(Object value,int length,String name) {
        if(!(value instanceof float[])||((float[])value).length!=length)
            throw new IllegalArgumentException(name+" expects exactly "+length+" float values");
        return (float[])value;
    }
    private static void rectangle(int[] box,String name,boolean normalized) {
        int limit=normalized?1:DECORATION_SCALE;
        float[] values=new float[]{box[0],box[1],box[2],box[3]};
        for(float component:values)finiteRange(component,0f,limit,name);
        if(!(box[0]<box[2]&&box[1]<box[3]))
            throw new IllegalArgumentException(name+" requires left<right and top<bottom");
    }
    private static void rectangle(float[] box,String name,boolean normalized) {
        int limit=normalized?1:DECORATION_SCALE;
        for(float component:box)finiteRange(component,0f,limit,name);
        if(!(box[0]<box[2]&&box[1]<box[3]))
            throw new IllegalArgumentException(name+" requires left<right and top<bottom");
    }
    private static void comment(Object value,String name,boolean allowEmpty) {
        if(!(value instanceof byte[]))throw new IllegalArgumentException(name+" expects a byte[] UTF-8 value");
        byte[] bytes=(byte[])value;
        if(bytes.length>(allowEmpty?MAX_TEXT_BYTES:MAX_TEXT_BYTES)||(!allowEmpty&&bytes.length==0))
            throw new IllegalArgumentException(name+" must be "+(allowEmpty?"at most ":"1..")+MAX_TEXT_BYTES+" UTF-8 bytes");
        for(byte item:bytes)if(item==0)throw new IllegalArgumentException(name+" must not contain a NUL byte");
        try {
            StandardCharsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT).decode(java.nio.ByteBuffer.wrap(bytes));
        } catch(CharacterCodingException failure) {
            throw new IllegalArgumentException(name+" is not valid UTF-8");
        }
    }

    // ---- standard key math (bounded, deterministic) ----
    /** Standard CONTRAST_CURVE points for brightness (-100..100) and contrast
     * (-100..100): black/white endpoints preserved, five control points. */
    static TonemapCurve tonemapCurve(int brightness,int contrast) {
        float[] points=tonemapPoints(brightness,contrast);
        return new TonemapCurve(points,points.clone(),points.clone());
    }
    static float[] tonemapPoints(int brightness,int contrast) {
        range(brightness,-100,100,"brightness");
        range(contrast,-100,100,"contrast");
        float gain=1f+contrast/100f,pivot=.5f,offset=brightness/100f*.25f;
        float[] points=new float[10];float previous=0;
        for(int i=0;i<5;++i) {
            float x=i*.25f;
            float y=Math.max(0f,Math.min(1f,pivot+(x-pivot)*gain+offset));
            if(i>0)y=Math.max(previous,y);
            if(i==4)y=1f;
            points[i*2]=x;points[i*2+1]=y;previous=y;
        }
        return points;
    }
    /** Standard COLOR_CORRECTION_TRANSFORM for saturation and hue (-100..100).
     * Hue maps linearly to +/-30 degrees of chroma rotation; saturation maps to
     * 0..2 around luma. Both are applied in the linear matrix domain. */
    static ColorSpaceTransform colorMatrix(int saturation,int hue) {
        range(saturation,-100,100,"saturation");
        range(hue,-100,100,"hue");
        double amount=1+saturation/100.0;
        double angle=Math.toRadians(hue*0.30);
        double cos=Math.cos(angle),sin=Math.sin(angle);
        // RGB -> YCbCr (BT.601) then rotate Cb/Cr, then YCbCr -> RGB.
        double[][] toYcc={{0.299,0.587,0.114},{-0.168736,-0.331264,0.5},{0.5,-0.418688,-0.081312}};
        double[][] toRgb={{1,0,1.402},{1,-0.344136,-0.714136},{1,1.772,0}};
        double[][] rotate={{1,0,0},{0,cos,-sin},{0,sin,cos}};
        double[][] scale={{1,0,0},{0,amount,0},{0,0,amount}};
        double[][] composed=multiply(toRgb,multiply(rotate,multiply(scale,toYcc)));
        Rational[] elements=new Rational[9];
        for(int i=0;i<9;++i)elements[i]=new Rational((int)Math.round(composed[i/3][i%3]*1000),1000);
        return new ColorSpaceTransform(elements);
    }
    /** Standard COLOR_CORRECTION_GAINS for red/blue gain (-100..100). */
    static RggbChannelVector whiteBalanceGains(int red,int blue) {
        range(red,-100,100,"rGain");
        range(blue,-100,100,"bGain");
        return new RggbChannelVector(1f+red/100f,1f,1f,1f+blue/100f);
    }
    private static double[][] multiply(double[][] a,double[][] b) {
        double[][] out=new double[3][3];
        for(int row=0;row<3;++row)for(int column=0;column<3;++column) {
            double sum=0;
            for(int k=0;k<3;++k)sum+=a[row][k]*b[k][column];
            out[row][column]=sum;
        }
        return out;
    }

    /** The advertised result counterpart of a surface request key, or null. */
    public static CaptureResult.Key<?> resultFor(CaptureRequest.Key<?> key) {
        return key==null?null:SNAPSHOT_RESULTS.get(key.getName());
    }

    /** Every request key the surface knows, for diagnostics and validation UIs. */
    public static List<CaptureRequest.Key<?>> knownRequests(){return ALL_REQUESTS;}
    /** Every transport-only result key the surface knows. */
    public static Set<String> knownResultNames() {
        Set<String> names=new LinkedHashSet<>();
        for(CaptureResult.Key<?> key:SURFACE_RESULTS)names.add(key.getName());
        names.addAll(SNAPSHOT_RESULTS.keySet());
        return Collections.unmodifiableSet(names);
    }
}
