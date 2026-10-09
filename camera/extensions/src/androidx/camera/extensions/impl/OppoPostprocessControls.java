/* SPDX-License-Identifier: BSD-3-Clause */
package androidx.camera.extensions.impl;

import android.hardware.camera2.CaptureRequest;
import android.hardware.camera2.CaptureResult;
import java.util.Arrays;
import java.util.List;
import java.util.Map;

/** Optional controls for advanced FACE_RETOUCH photo/preview postprocessing.
 * See camera/extensions/POSTPROCESS_CONTROLS.md and device-tree-nonsource-20261017/camera/docs/EXTENSIONS_SURFACE.md
 * for versioned payload contracts. Standard EXTENSION_STRENGTH gates/scales all
 * edits; zero bypasses them. The v1 controls below keep their exact names and
 * semantics; {@code watermarkLayout}, {@code mirror} and {@code exifUserComment}
 * are the round-2 additions of contract section 3.2. */
public final class OppoPostprocessControls {
    private OppoPostprocessControls() {}
    public static final CaptureRequest.Key<int[]> SETTINGS=new CaptureRequest.Key<>(
            "org.lineageos.camera.postprocess.settings",int[].class);
    public static final CaptureRequest.Key<float[]> WARP_REGIONS=new CaptureRequest.Key<>(
            "org.lineageos.camera.postprocess.warpRegions",float[].class);
    public static final CaptureRequest.Key<int[]> OVERLAY=new CaptureRequest.Key<>(
            "org.lineageos.camera.postprocess.overlay",int[].class);
    public static final CaptureRequest.Key<byte[]> WATERMARK=new CaptureRequest.Key<>(
            "org.lineageos.camera.postprocess.watermark",byte[].class);
    public static final CaptureRequest.Key<int[]> FACE_TRACKING=new CaptureRequest.Key<>(
            "org.lineageos.camera.postprocess.faceTracking",int[].class);
    /** Provider-owned decoration box, ten-thousandths of the final output. */
    public static final CaptureRequest.Key<int[]> WATERMARK_LAYOUT=OppoSurfaceControls.WATERMARK_LAYOUT;
    /** Provider-owned discrete mirror, equivalent to {@code settings[6]}. */
    public static final CaptureRequest.Key<Integer> MIRROR=OppoSurfaceControls.MIRROR;
    /** Provider-owned standard EXIF UserComment (also written when the provider
     * encodes the JPEG itself). */
    public static final CaptureRequest.Key<byte[]> EXIF_USER_COMMENT=OppoSurfaceControls.EXIF_USER_COMMENT;
    public static final CaptureResult.Key<int[]> SETTINGS_RESULT=new CaptureResult.Key<>(
            "org.lineageos.camera.postprocess.settings",int[].class);
    public static final CaptureResult.Key<float[]> WARP_REGIONS_RESULT=new CaptureResult.Key<>(
            "org.lineageos.camera.postprocess.warpRegions",float[].class);
    public static final CaptureResult.Key<int[]> OVERLAY_RESULT=new CaptureResult.Key<>(
            "org.lineageos.camera.postprocess.overlay",int[].class);
    public static final CaptureResult.Key<byte[]> WATERMARK_RESULT=new CaptureResult.Key<>(
            "org.lineageos.camera.postprocess.watermark",byte[].class);
    public static final CaptureResult.Key<int[]> FACE_TRACKING_RESULT=new CaptureResult.Key<>(
            "org.lineageos.camera.postprocess.faceTracking",int[].class);
    static final List<CaptureRequest.Key> REQUESTS=Arrays.asList(SETTINGS,WARP_REGIONS,OVERLAY,WATERMARK);
    static final List<CaptureResult.Key> RESULTS=Arrays.asList(SETTINGS_RESULT,WARP_REGIONS_RESULT,OVERLAY_RESULT,WATERMARK_RESULT);
    static PortraitPostprocess.Options options(Map<CaptureRequest.Key<?>,Object> controls) {
        Object settings=controls.get(SETTINGS),warps=controls.get(WARP_REGIONS);
        Object overlay=controls.get(OVERLAY),text=controls.get(WATERMARK),tracking=controls.get(FACE_TRACKING);
        Object layout=OppoSurfaceControls.find(controls,WATERMARK_LAYOUT);
        Object mirror=OppoSurfaceControls.find(controls,MIRROR);
        if((settings!=null&&!(settings instanceof int[]))||(warps!=null&&!(warps instanceof float[]))||
                (overlay!=null&&!(overlay instanceof int[]))||(text!=null&&!(text instanceof byte[]))||
                (tracking!=null&&!(tracking instanceof int[])))
            throw new IllegalArgumentException("Invalid postprocess payload type");
        if(layout!=null&&!(layout instanceof int[]))
            throw new IllegalArgumentException("watermarkLayout expects an int32[4] value");
        if(mirror!=null&&!(mirror instanceof Integer))
            throw new IllegalArgumentException("mirror expects an int32 0/1 value");
        int[] values=(int[])settings;
        if(mirror!=null) {
            int requested=(Integer)mirror;
            if(requested<0||requested>1)throw new IllegalArgumentException("mirror must be 0 or 1");
            if(values!=null&&values.length==PortraitPostprocess.SETTINGS_COUNT&&values[6]!=0&&values[6]!=requested)
                throw new IllegalArgumentException("Conflicting mirror controls: settings[6] and postprocess.mirror differ");
            values=values==null?null:values.clone();
            if(values==null) {
                values=PortraitPostprocess.DEFAULTS.clone();
            }
            values[6]=requested;
        }
        return new PortraitPostprocess.Options(values,(float[])warps,(int[])overlay,(byte[])text,(int[])tracking,
            (int[])layout);
    }
    static void strip(Map<CaptureRequest.Key<?>,Object> controls) {
        for(CaptureRequest.Key<?> key:REQUESTS)controls.remove(key);
        controls.remove(FACE_TRACKING);
        OppoSurfaceControls.stripProvider(controls);
    }
    static void results(Map<CaptureResult.Key,Object> values,Map<CaptureRequest.Key<?>,Object> controls) {
        PortraitPostprocess.Options options=options(controls);
        values.put(SETTINGS_RESULT,options.settings.clone());values.put(WARP_REGIONS_RESULT,options.warps.clone());
        values.put(OVERLAY_RESULT,options.overlay.clone());values.put(WATERMARK_RESULT,options.text.clone());
        values.put(FACE_TRACKING_RESULT,options.tracking.clone());
    }
}
