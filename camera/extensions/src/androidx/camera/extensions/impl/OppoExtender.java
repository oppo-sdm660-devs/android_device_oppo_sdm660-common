/* SPDX-License-Identifier: BSD-3-Clause */
package androidx.camera.extensions.impl;

import android.content.Context;
import android.graphics.ImageFormat;
import android.graphics.Rect;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CaptureRequest;
import android.hardware.camera2.CaptureResult;
import android.hardware.camera2.params.SessionConfiguration;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.util.Pair;
import android.util.Range;
import android.util.Size;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;

abstract class OppoExtender implements ExtenderStateListener {
    enum Mode { AUTO, RETOUCH, HDR, NIGHT, BOKEH }
    final Mode mode;
    CameraCharacteristics characteristics;
    OppoImageProcessor processor;
    OppoExtender(Mode m) { mode=m; }
    final boolean processed() { return mode==Mode.AUTO || mode==Mode.RETOUCH || mode==Mode.BOKEH; }
    public void init(String id,CameraCharacteristics c) { characteristics=c; }
    public void onInit(String id,CameraCharacteristics c,Context context) { init(id,c); }
    public void onDeInit() { if(processor!=null)processor.close(); processor=null; }
    public CaptureStageImpl onPresetSession() { return stage(0,false); }
    public CaptureStageImpl onEnableSession() { return stage(0,false); }
    public CaptureStageImpl onDisableSession() { return null; }
    public int onSessionType() { return SessionConfiguration.SESSION_REGULAR; }
    static boolean contains(int[] values,int value) {
        if(values!=null)for(int item:values)if(item==value)return true;
        return false;
    }
    public boolean isExtensionAvailable(String id,CameraCharacteristics c) {
        if(c==null || !("0".equals(id)||"1".equals(id)))return false;
        StreamConfigurationMap map=c.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
        if(map==null||map.getOutputSizes(ImageFormat.JPEG)==null)return false;
        if(mode==Mode.HDR||mode==Mode.NIGHT)return contains(c.get(CameraCharacteristics.CONTROL_AVAILABLE_SCENE_MODES),
            mode==Mode.HDR?CaptureRequest.CONTROL_SCENE_MODE_HDR:CaptureRequest.CONTROL_SCENE_MODE_NIGHT);
        if(!hasCommonSize(map,ImageFormat.JPEG,PixelPipeline.MAX_PIXELS)||
                !hasCommonSize(map,ImageFormat.PRIVATE,1280*720))return false;
        if(mode==Mode.RETOUCH||mode==Mode.BOKEH)return contains(c.get(CameraCharacteristics.STATISTICS_INFO_AVAILABLE_FACE_DETECT_MODES),
            CaptureRequest.STATISTICS_FACE_DETECT_MODE_SIMPLE);
        return true;
    }
    private static boolean hasCommonSize(StreamConfigurationMap map,int format,int maximumPixels) {
        Size[] input=map.getOutputSizes(ImageFormat.YUV_420_888),output=map.getOutputSizes(format);
        if(input==null||output==null)return false;
        for(Size size:output)if(size.getWidth()>=32&&size.getHeight()>=32&&
                (size.getWidth()&1)==0&&(size.getHeight()&1)==0&&
                (long)size.getWidth()*size.getHeight()<=maximumPixels&&Arrays.asList(input).contains(size))return true;
        return false;
    }
    CaptureStageImpl stage(final int id,boolean burst) {
        List<Pair<CaptureRequest.Key,Object>> values=new ArrayList<>();
        if(mode==Mode.HDR||mode==Mode.NIGHT) {
            values.add(Pair.create(CaptureRequest.CONTROL_MODE,CaptureRequest.CONTROL_MODE_USE_SCENE_MODE));
            values.add(Pair.create(CaptureRequest.CONTROL_SCENE_MODE,mode==Mode.HDR?
                CaptureRequest.CONTROL_SCENE_MODE_HDR:CaptureRequest.CONTROL_SCENE_MODE_NIGHT));
        } else if(processed()) {
            values.add(Pair.create(CaptureRequest.CONTROL_MODE,CaptureRequest.CONTROL_MODE_AUTO));
            if(characteristics!=null&&contains(characteristics.get(CameraCharacteristics.STATISTICS_INFO_AVAILABLE_FACE_DETECT_MODES),
                CaptureRequest.STATISTICS_FACE_DETECT_MODE_SIMPLE))
                values.add(Pair.create(CaptureRequest.STATISTICS_FACE_DETECT_MODE,CaptureRequest.STATISTICS_FACE_DETECT_MODE_SIMPLE));
            if(burst&&characteristics!=null) {
                if(Boolean.TRUE.equals(characteristics.get(CameraCharacteristics.CONTROL_AE_LOCK_AVAILABLE)))
                    values.add(Pair.create(CaptureRequest.CONTROL_AE_LOCK,true));
                if(Boolean.TRUE.equals(characteristics.get(CameraCharacteristics.CONTROL_AWB_LOCK_AVAILABLE)))
                    values.add(Pair.create(CaptureRequest.CONTROL_AWB_LOCK,true));
            }
            // AF/AE triggers are pulses, not settings to repeat on every
            // member of the four-frame capture advertised by the basic API.
            if(burst&&id>0) {
                values.add(Pair.create(CaptureRequest.CONTROL_AF_TRIGGER,CaptureRequest.CONTROL_AF_TRIGGER_IDLE));
                values.add(Pair.create(CaptureRequest.CONTROL_AE_PRECAPTURE_TRIGGER,CaptureRequest.CONTROL_AE_PRECAPTURE_TRIGGER_IDLE));
            }
        }
        return new CaptureStageImpl() {
            public int getId() { return id; }
            public List<Pair<CaptureRequest.Key,Object>> getParameters() { return values; }
        };
    }
    List<Pair<Integer,Size[]>> resolutions(boolean preview) {
        List<Pair<Integer,Size[]>> result=new ArrayList<>();
        if(characteristics==null)return result;
        StreamConfigurationMap map=characteristics.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
        if(map==null)return result;
        int[] formats=preview?new int[]{ImageFormat.PRIVATE,ImageFormat.YUV_420_888}:
            new int[]{ImageFormat.JPEG,ImageFormat.YUV_420_888};
        Size[] input=map.getOutputSizes(ImageFormat.YUV_420_888);
        for(int format:formats) {
            Size[] sizes=map.getOutputSizes(format);if(sizes==null)continue;
            List<Size> supported=new ArrayList<>();
            for(Size size:sizes) {
                if(processed()) {
                    if(size.getWidth()<32||size.getHeight()<32||(size.getWidth()&1)!=0||(size.getHeight()&1)!=0||
                        (long)size.getWidth()*size.getHeight()>(preview?1280*720:PixelPipeline.MAX_PIXELS))continue;
                    if(input==null||!Arrays.asList(input).contains(size))continue;
                } else if(!preview&&format!=ImageFormat.JPEG)continue;
                supported.add(size);
            }
            if(!supported.isEmpty())result.add(Pair.create(format,supported.toArray(new Size[0])));
        }
        return result;
    }
    OppoImageProcessor processor() {
        if(processor==null)processor=new OppoImageProcessor(mode,characteristics);
        return processor;
    }
    // Only advertise controls whose result values can be returned with the anchor.
    static final List<CaptureRequest.Key> REQUEST_KEYS=Arrays.asList(
        CaptureRequest.SCALER_CROP_REGION,CaptureRequest.CONTROL_AF_MODE,
        CaptureRequest.CONTROL_AF_TRIGGER,CaptureRequest.CONTROL_AF_REGIONS,
        CaptureRequest.CONTROL_AE_REGIONS,CaptureRequest.CONTROL_AWB_REGIONS,
        CaptureRequest.CONTROL_AE_PRECAPTURE_TRIGGER,
        CaptureRequest.CONTROL_AE_EXPOSURE_COMPENSATION,CaptureRequest.JPEG_ORIENTATION,
        CaptureRequest.JPEG_QUALITY);
    // Real Camera2 tags. The SDK hides these constants, but its public typed
    // key constructor resolves the same metadata fields. STATISTICS_FACES is
    // synthetic and cannot be passed to the extension proxy's getTag list.
    static final CaptureResult.Key<Rect[]> FACE_RECTANGLES=new CaptureResult.Key<>(
        "android.statistics.faceRectangles",Rect[].class);
    static final CaptureResult.Key<byte[]> FACE_SCORES=new CaptureResult.Key<>(
        "android.statistics.faceScores",byte[].class);
    static final CaptureResult.Key<int[]> FACE_IDS=new CaptureResult.Key<>(
        "android.statistics.faceIds",int[].class);
    static final CaptureResult.Key<int[]> FACE_LANDMARKS=new CaptureResult.Key<>(
        "android.statistics.faceLandmarks",int[].class);
    static final List<CaptureResult.Key> RESULT_KEYS=Arrays.asList(
        CaptureResult.SCALER_CROP_REGION,CaptureResult.CONTROL_AF_MODE,
        CaptureResult.CONTROL_AF_TRIGGER,CaptureResult.CONTROL_AF_REGIONS,
        CaptureResult.CONTROL_AE_REGIONS,CaptureResult.CONTROL_AWB_REGIONS,
        CaptureResult.CONTROL_AE_PRECAPTURE_TRIGGER,CaptureResult.CONTROL_AE_MODE,
        CaptureResult.CONTROL_AE_LOCK,CaptureResult.CONTROL_AWB_MODE,CaptureResult.CONTROL_AWB_LOCK,
        CaptureResult.CONTROL_AF_STATE,CaptureResult.CONTROL_AE_STATE,CaptureResult.CONTROL_AWB_STATE,
        CaptureResult.COLOR_CORRECTION_MODE,CaptureResult.COLOR_CORRECTION_GAINS,CaptureResult.COLOR_CORRECTION_TRANSFORM,
        // CameraMetadataNative.setFaces does not set its detection mode. Keep
        // the actual anchor mode so reconstruction retains FULL IDs/points.
        CaptureResult.FLASH_MODE,CaptureResult.FLASH_STATE,CaptureResult.STATISTICS_FACE_DETECT_MODE,
        CaptureResult.CONTROL_AE_EXPOSURE_COMPENSATION,CaptureResult.JPEG_ORIENTATION,
        CaptureResult.JPEG_QUALITY,CaptureResult.SENSOR_TIMESTAMP,
        CaptureResult.SENSOR_EXPOSURE_TIME,CaptureResult.SENSOR_SENSITIVITY,CaptureResult.SENSOR_FRAME_DURATION,
        // Standard round-2 surface results (zoom/multi-camera switching, tone
        // map, sharpness, hot-pixel map mode).
        CaptureResult.CONTROL_ZOOM_RATIO,CaptureResult.LOGICAL_MULTI_CAMERA_ACTIVE_PHYSICAL_ID,
        CaptureResult.TONEMAP_MODE,CaptureResult.TONEMAP_CURVE,OppoSurfaceControls.SHARPNESS_RESULT,
        CaptureResult.STATISTICS_HOT_PIXEL_MAP_MODE);
    List<CaptureResult.Key> resultKeys() {
        return resultKeys(characteristics,mode.ordinal(),false);
    }
    static List<CaptureResult.Key> resultKeys(CameraCharacteristics characteristics) {
        return resultKeys(characteristics,-1,false);
    }
    static List<CaptureResult.Key> resultKeys(CameraCharacteristics characteristics,int extensionMode,
            boolean advanced) {
        if(characteristics==null)return Collections.emptyList();
        List<CaptureResult.Key> keys=new ArrayList<>(RESULT_KEYS);
        if(extensionMode>=0)for(CaptureResult.Key<?> key:OppoSurfaceControls.availableResults(characteristics,extensionMode))
            if(advanced||OppoSurfaceControls.standardResult(key))keys.add(key);
        keys.retainAll(characteristics.getAvailableCaptureResultKeys());
        int[] modes=characteristics.get(CameraCharacteristics.STATISTICS_INFO_AVAILABLE_FACE_DETECT_MODES);
        // Native face tags are hidden from the public characteristics key
        // filter. The public synthetic face capability and mode list establish
        // their backing fields without advertising the synthetic key itself.
        if(characteristics.getAvailableCaptureResultKeys().contains(CaptureResult.STATISTICS_FACES)&&
                (contains(modes,CaptureRequest.STATISTICS_FACE_DETECT_MODE_SIMPLE)||
                 contains(modes,CaptureRequest.STATISTICS_FACE_DETECT_MODE_FULL))) {
            keys.add(FACE_RECTANGLES);keys.add(FACE_SCORES);
            if(contains(modes,CaptureRequest.STATISTICS_FACE_DETECT_MODE_FULL)) {
                keys.add(FACE_IDS);keys.add(FACE_LANDMARKS);
            }
        }
        return keys;
    }
    List<CaptureRequest.Key> requestKeys(boolean advanced) {
        if(characteristics==null)return Collections.emptyList();
        List<CaptureRequest.Key> keys=new ArrayList<>(REQUEST_KEYS);
        // HDR/NIGHT own scene-mode 3A. Processed extensions preserve orthogonal
        // presets; advanced sessions additionally own flash burst selection.
        if(processed()) {
            keys.add(CaptureRequest.CONTROL_AWB_MODE);
            if(contains(characteristics.get(CameraCharacteristics.CONTROL_AWB_AVAILABLE_MODES),CaptureRequest.CONTROL_AWB_MODE_OFF)) {
                keys.add(CaptureRequest.COLOR_CORRECTION_MODE);keys.add(CaptureRequest.COLOR_CORRECTION_GAINS);
                keys.add(CaptureRequest.COLOR_CORRECTION_TRANSFORM);
            }
            if(advanced) {
                keys.add(CaptureRequest.CONTROL_AE_MODE);keys.add(CaptureRequest.FLASH_MODE);
                keys.add(CaptureRequest.CONTROL_AE_LOCK);keys.add(CaptureRequest.CONTROL_AWB_LOCK);
                if(contains(characteristics.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES),
                        CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR)&&
                        contains(characteristics.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_MODES),
                        CaptureRequest.CONTROL_AE_MODE_OFF)) {
                    keys.add(CaptureRequest.SENSOR_EXPOSURE_TIME);keys.add(CaptureRequest.SENSOR_SENSITIVITY);
                    keys.add(CaptureRequest.SENSOR_FRAME_DURATION);
                }
            }
        }
        keys.retainAll(characteristics.getAvailableCaptureRequestKeys());
        // Every advertised request must have a result counterpart returned
        // with the actual anchor. HALs can expose asymmetric key lists.
        List<CaptureResult.Key> results=resultKeys();
        keys.removeIf(key->{
            for(CaptureResult.Key result:results)if(key.getName().equals(result.getName()))return false;
            return true;
        });
        // Round-2 surface: standard Camera2 controls are advertised for every
        // extension that can carry them. Vendor controls and provider-owned
        // postprocess controls only reach an application through the advanced
        // parameter channel, so basic Extensions advertise the standard keys.
        // availableRequests() already matches the camera's advertised keys by
        // name; the framework transports client-created key instances, so a
        // name comparison is the only reliable test here.
        for(CaptureRequest.Key<?> key:OppoSurfaceControls.availableRequests(characteristics,mode.ordinal()))
            if((advanced||OppoSurfaceControls.standard(key))&&!keys.contains(key))keys.add(key);
        return keys;
    }
}

abstract class OppoCaptureExtender extends OppoExtender implements ImageCaptureExtenderImpl {
    OppoCaptureExtender(Mode m) { super(m); }
    public List<CaptureStageImpl> getCaptureStages() {
        List<CaptureStageImpl> result=new ArrayList<>();
        for(int i=0;i<getMaxCaptureStage();++i)result.add(stage(i,processed()));
        return result;
    }
    public int getMaxCaptureStage() { return processed()?4:1; }
    public CaptureProcessorImpl getCaptureProcessor() { return processed()?processor():null; }
    public List<Pair<Integer,Size[]>> getSupportedResolutions() { return resolutions(false); }
    public List<Pair<Integer,Size[]>> getSupportedPostviewResolutions(Size size) { return Collections.emptyList(); }
    public Range<Long> getEstimatedCaptureLatencyRange(Size size) { return null; }
    public List<CaptureRequest.Key> getAvailableCaptureRequestKeys() {
        return requestKeys(false);
    }
    public List<CaptureResult.Key> getAvailableCaptureResultKeys() {
        return resultKeys();
    }
    public boolean isCaptureProcessProgressAvailable() { return false; }
    public Pair<Long,Long> getRealtimeCaptureLatency() { return null; }
    public boolean isPostviewAvailable() { return false; }
}

abstract class OppoPreviewExtender extends OppoExtender implements PreviewExtenderImpl {
    OppoPreviewExtender(Mode m) { super(m); }
    public CaptureStageImpl getCaptureStage() { return stage(0,false); }
    public ProcessorType getProcessorType() {
        return mode==Mode.RETOUCH||mode==Mode.BOKEH?ProcessorType.PROCESSOR_TYPE_IMAGE_PROCESSOR:ProcessorType.PROCESSOR_TYPE_NONE;
    }
    public ProcessorImpl getProcessor() { return mode==Mode.RETOUCH||mode==Mode.BOKEH?processor():null; }
    public List<Pair<Integer,Size[]>> getSupportedResolutions() { return resolutions(true); }
}
