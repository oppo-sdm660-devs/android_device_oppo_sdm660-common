/* SPDX-License-Identifier: BSD-3-Clause */
package androidx.camera.extensions.impl;

import android.graphics.ImageFormat;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CaptureRequest;
import android.hardware.camera2.CaptureResult;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.util.Pair;
import android.util.Range;
import android.util.Size;
import androidx.camera.extensions.impl.advanced.AdvancedExtenderImpl;
import androidx.camera.extensions.impl.advanced.SessionProcessorImpl;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

public class OppoAdvancedExtender extends OppoExtender implements AdvancedExtenderImpl {
    static final CameraCharacteristics.Key<int[]> NATIVE_MODES = new CameraCharacteristics.Key<>(
        "org.lineageos.camera.algorithms.availableModes",int[].class);
    static final CaptureRequest.Key<Integer> NATIVE_STILL_MODE = new CaptureRequest.Key<>(
        "org.lineageos.camera.algorithms.stillMode",Integer.class);
    static final CaptureRequest.Key<Integer> NATIVE_BEAUTY_STRENGTH = new CaptureRequest.Key<>(
        "org.lineageos.camera.algorithms.beautyStrength",Integer.class);
    static final CaptureRequest.Key<Integer> NATIVE_SCENE_STRENGTH = new CaptureRequest.Key<>(
        "org.lineageos.camera.algorithms.sceneStrength",Integer.class);
    static final CameraCharacteristics.Key<int[]> AVAILABLE_FILTER_TYPES = new CameraCharacteristics.Key<>(
        "org.lineageos.camera.algorithms.availableFilterTypes",int[].class);
    static final CameraCharacteristics.Key<int[]> CONTINUOUS_AVAILABLE_EFFECTS = new CameraCharacteristics.Key<>(
        "org.lineageos.camera.algorithms.continuousAvailableEffects",int[].class);
    public OppoAdvancedExtender(int mode) { super(Mode.values()[mode]); }
    public void init(String id,Map<String,CameraCharacteristics> values) { init(id,values.get(id)); }
    @Override public boolean isExtensionAvailable(String id,CameraCharacteristics value) {
        // Only the advanced provider can give paired JPEG ownership to HAL3.
        // Basic BOKEH remains the independent single-camera implementation.
        if("3".equals(id)) {
            if(mode!=Mode.BOKEH||value==null)return false;
            int[] modes;
            try { modes=value.get(NATIVE_MODES); }
            catch(IllegalArgumentException unsupported) { return false; }
            StreamConfigurationMap config=value.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
            return contains(modes,7)&&config!=null&&config.getOutputSizes(ImageFormat.JPEG)!=null;
        }
        return super.isExtensionAvailable(id,value);
    }
    public boolean isExtensionAvailable(String id,Map<String,CameraCharacteristics> values) {
        if(!isExtensionAvailable(id,values.get(id)))return false;
        // Availability is queried before init. Use an independent capability
        // probe, so querying another camera never changes a live session's
        // metadata. Advanced providers must support JPEG and YUV capture.
        OppoAdvancedExtender probe=new OppoAdvancedExtender(mode.ordinal());probe.init(id,values);
        if(processed()&&!JpegBlobWriter.available()&&probe.nativeMode()==0)return false;
        Map<Integer,List<Size>> capture=probe.getSupportedCaptureOutputResolutions(id);
        Map<Integer,List<Size>> preview=probe.getSupportedPreviewOutputResolutions(id);
        return nonempty(capture.get(ImageFormat.JPEG))&&nonempty(capture.get(ImageFormat.YUV_420_888))&&
            nonempty(preview.get(ImageFormat.PRIVATE));
    }
    private static boolean nonempty(List<Size> sizes) { return sizes!=null&&!sizes.isEmpty(); }
    private int[] nativeModes() {
        if(characteristics==null)return null;
        // Older camera providers do not register these vendor tags.
        try { return characteristics.get(NATIVE_MODES); }
        catch(IllegalArgumentException unsupported) { return null; }
    }
    boolean nativeControlsAvailable() {
        int[] modes=nativeModes();return modes!=null&&modes.length>0;
    }
    boolean supportsNativeMode(int value) { return contains(nativeModes(),value); }
    int nativeMode() {
        int[] modes=nativeModes();
        switch(mode) {
            case HDR:return contains(modes,1)?1:0;
            case NIGHT:return contains(modes,2)?2:0;
            case AUTO:return contains(modes,4)?4:(contains(modes,3)?3:(contains(modes,9)?9:0));
            case RETOUCH:return contains(modes,5)?5:0;
            case BOKEH:return contains(modes,7)?7:0;
            default:return 0;
        }
    }
    public Range<Long> getEstimatedCaptureLatencyRange(String id,Size size,int format) { return null; }
    public Map<Integer,List<Size>> getSupportedPreviewOutputResolutions(String id) {
        return map(resolutions(true));
    }
    private static Map<Integer,List<Size>> map(List<Pair<Integer,Size[]>> pairs) {
        Map<Integer,List<Size>> result=new LinkedHashMap<>();
        for(Pair<Integer,Size[]> pair:pairs)result.put(pair.first,Arrays.asList(pair.second));return result;
    }
    Size inputFor(Size output) {
        if(characteristics==null||output==null||output.getWidth()<32||output.getHeight()<32||
                (output.getWidth()&1)!=0||(output.getHeight()&1)!=0)return null;
        StreamConfigurationMap map=characteristics.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
        Size[] inputs=map==null?null:map.getOutputSizes(ImageFormat.YUV_420_888);
        if(inputs==null)return null;
        long pixels=(long)output.getWidth()*output.getHeight();
        if(mode==Mode.AUTO&&(output.getWidth()%4)==0&&(output.getHeight()%4)==0&&
                pixels<=SuperResolution.MAX_OUTPUT_PIXELS) {
            Size half=new Size(output.getWidth()/2,output.getHeight()/2);
            if(half.getWidth()>=32&&half.getHeight()>=32&&Arrays.asList(inputs).contains(half))return half;
        }
        return pixels<=PixelPipeline.MAX_PIXELS&&Arrays.asList(inputs).contains(output)?output:null;
    }
    public Map<Integer,List<Size>> getSupportedCaptureOutputResolutions(String id) {
        Map<Integer,List<Size>> result=softwareCaptureResolutions();
        if(nativeMode()!=0&&characteristics!=null) {
            StreamConfigurationMap config=characteristics.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
            Size[] declared=config==null?null:config.getOutputSizes(ImageFormat.JPEG);
            List<Size> jpeg=new ArrayList<>();
            if(declared!=null)for(Size size:declared)
                if(size.getWidth()>=32&&size.getHeight()>=32&&(size.getWidth()&1)==0&&(size.getHeight()&1)==0)
                    jpeg.add(size);
            if(!jpeg.isEmpty())result.put(ImageFormat.JPEG,jpeg);
        }
        return result;
    }
    private Map<Integer,List<Size>> softwareCaptureResolutions() {
        if(mode!=Mode.AUTO) {
            Map<Integer,List<Size>> result=map(resolutions(false));
            if(!processed()&&characteristics!=null) {
                StreamConfigurationMap config=characteristics.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
                Size[] yuv=config==null?null:config.getOutputSizes(ImageFormat.YUV_420_888);
                List<Size> jpeg=result.get(ImageFormat.JPEG),sizes=new ArrayList<>();
                if(yuv!=null&&jpeg!=null)for(Size size:yuv)
                    if(size.getWidth()>=32&&size.getHeight()>=32&&(size.getWidth()&1)==0&&(size.getHeight()&1)==0&&
                            (long)size.getWidth()*size.getHeight()<=PixelPipeline.MAX_PIXELS&&jpeg.contains(size))sizes.add(size);
                if(!sizes.isEmpty())result.put(ImageFormat.YUV_420_888,sizes);
            }
            return result;
        }
        Map<Integer,List<Size>> result=new LinkedHashMap<>();
        if(characteristics==null)return result;
        StreamConfigurationMap config=characteristics.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
        if(config==null)return result;
        for(int format:new int[]{ImageFormat.JPEG,ImageFormat.YUV_420_888}) {
            List<Size> sizes=new ArrayList<>();Size[] declared=config.getOutputSizes(format);
            if(declared!=null)for(Size size:declared)if(inputFor(size)!=null)sizes.add(size);
            if(!sizes.isEmpty())result.put(format,sizes);
        }
        return result;
    }
    public Map<Integer,List<Size>> getSupportedPostviewResolutions(Size size) { return Collections.emptyMap(); }
    public List<Size> getSupportedYuvAnalysisResolutions(String id) { return Collections.emptyList(); }
    public SessionProcessorImpl createSessionProcessor() { return new OppoSessionProcessor(this); }
    public List<Pair<CameraCharacteristics.Key,Object>> getAvailableCharacteristicsKeyValues() { return Collections.emptyList(); }
    public List<CaptureRequest.Key> getAvailableCaptureRequestKeys() {
        List<CaptureRequest.Key> keys=new ArrayList<>(requestKeys(true));
        if(mode==Mode.RETOUCH||mode==Mode.BOKEH)keys.add(CaptureRequest.EXTENSION_STRENGTH);
        if(mode==Mode.RETOUCH)keys.addAll(OppoPostprocessControls.REQUESTS);
        if(mode==Mode.RETOUCH&&fullFaces())keys.add(OppoPostprocessControls.FACE_TRACKING);
        return keys;
    }
    public List<CaptureResult.Key> getAvailableCaptureResultKeys() {
        List<CaptureResult.Key> keys=new ArrayList<>(resultKeys(characteristics,mode.ordinal(),true));
        if(mode==Mode.RETOUCH||mode==Mode.BOKEH)keys.add(CaptureResult.EXTENSION_STRENGTH);
        if(mode==Mode.RETOUCH)keys.addAll(OppoPostprocessControls.RESULTS);
        if(mode==Mode.RETOUCH&&fullFaces()) {
            keys.add(OppoPostprocessControls.FACE_TRACKING_RESULT);
            if(characteristics.getAvailableCaptureResultKeys().contains(OppoImageProcessor.BLINK_DEGREE))
                keys.add(OppoImageProcessor.BLINK_DEGREE);
        }
        return keys;
    }
    boolean fullFaces() {
        return characteristics!=null&&contains(characteristics.get(CameraCharacteristics.STATISTICS_INFO_AVAILABLE_FACE_DETECT_MODES),
            CaptureRequest.STATISTICS_FACE_DETECT_MODE_FULL);
    }
    public boolean isCaptureProcessProgressAvailable() { return false; }
    public boolean isPostviewAvailable() { return false; }
}
