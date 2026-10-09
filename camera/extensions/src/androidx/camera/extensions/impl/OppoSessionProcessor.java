/* SPDX-License-Identifier: BSD-3-Clause */
package androidx.camera.extensions.impl;

import android.content.Context;
import android.graphics.ImageFormat;
import android.graphics.Rect;
import android.graphics.YuvImage;
import android.hardware.HardwareBuffer;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CaptureFailure;
import android.hardware.camera2.CaptureRequest;
import android.hardware.camera2.CaptureResult;
import android.hardware.camera2.TotalCaptureResult;
import android.hardware.camera2.params.MeteringRectangle;
import android.media.Image;
import android.media.ImageWriter;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Pair;
import android.util.Size;
import androidx.camera.extensions.impl.advanced.*;
import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CancellationException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;

/** One bounded still transaction, copied input ownership and explicit outputs.
 * Result/image callbacks can arrive in either order; only matching timestamps
 * are paired. All ImageReferenceImpl instances are decremented in the callback.
 */
final class OppoSessionProcessor implements SessionProcessorImpl {
    private final OppoAdvancedExtender extender;
    private final Object lock=new Object();
    private final AtomicBoolean closed=new AtomicBoolean(true);
    private final AtomicInteger nextId=new AtomicInteger(1);
    private final OppoAutoCapturePolicy autoPolicy=new OppoAutoCapturePolicy();
    private final Map<CaptureRequest.Key<?>,Object> parameters=new HashMap<>();
    private final LinkedHashMap<Long,PreviewImage> previewImages=new LinkedHashMap<>();
    private final LinkedHashMap<Long,PreviewResult> previewResults=new LinkedHashMap<>();
    private ExecutorService worker;
    private ScheduledExecutorService timer;
    private HandlerThread releaseThread;
    private RequestProcessorImpl requests;
    private Camera2OutputConfigImpl previewConfig,captureConfig;
    private ImageWriter previewWriter,captureWriter;
    private Size previewSize,inputSize,outputSize;
    private int inputFormat=ImageFormat.YUV_420_888;
    private int nativeCaptureMode;
    private boolean postprocessJpeg;
    private FaceTracker faceTracker=new FaceTracker();
    private boolean initialized,previewBusy,captureJpeg,aborting,previewOutputPending,captureOutputPending;
    private volatile long generation;
    private volatile long repeatingGeneration;
    private volatile Burst burst;
    private PreviewSequence repeating;
    private OppoImageProcessor faceMapper;

    private static final class PreviewImage {
        final byte[] pixels;final long arrival;
        PreviewImage(byte[] copy,long received) { pixels=copy;arrival=received; }
    }
    private static final class PreviewResult {
        final TotalCaptureResult metadata;final long arrival;final boolean automatic;final int strength;
        final PortraitPostprocess.Options postprocess;
        PreviewResult(TotalCaptureResult value,long received,boolean eligible,int amount,PortraitPostprocess.Options edits) {
            metadata=value;arrival=received;automatic=eligible;strength=amount;postprocess=edits;
        }
    }

    private static final class PreviewSequence {
        final int id;final long generation;final CaptureCallback callback;final boolean repeating;
        long revision;boolean terminal,failed,notified;
        PreviewSequence(int sequence,long session,CaptureCallback cb,boolean repeat) {
            id=sequence;generation=session;callback=cb;repeating=repeat;
        }
    }

    private static final class Burst {
        final int id,count;final long generation;final CaptureCallback callback;
        final Map<CaptureRequest.Key<?>,Object> parameters;
        final AtomicBoolean cancelled=new AtomicBoolean();
        final LinkedHashMap<Long,byte[]> images=new LinkedHashMap<>();
        final TotalCaptureResult[] results;
        final long[] started;
        ScheduledFuture<?> timeout;
        boolean processing,terminal,sequenceCompleted,processStarted;
        Burst(int sequence,int n,long session,CaptureCallback cb,Map<CaptureRequest.Key<?>,Object> p) {
            id=sequence;count=n;generation=session;callback=cb;parameters=p;
            results=new TotalCaptureResult[n];started=new long[n];java.util.Arrays.fill(started,-1);
        }
    }
    private static final class Request implements RequestProcessorImpl.Request {
        final int stage,output,template;
        final Map<CaptureRequest.Key<?>,Object> values;
        Request(int index,int stream,int type,Map<CaptureRequest.Key<?>,Object> p) {
            stage=index;output=stream;template=type;values=p;
        }
        public List<Integer> getTargetOutputConfigIds() { return Collections.singletonList(output); }
        public Map<CaptureRequest.Key<?>,Object> getParameters() { return values; }
        public Integer getTemplateId() { return template; }
    }
    OppoSessionProcessor(OppoAdvancedExtender owner) { extender=owner; }
    public Camera2SessionConfigImpl initSession(String id,Map<String,CameraCharacteristics> cameras,
            Context context,OutputSurfaceConfigurationImpl surfaces) {
        if(surfaces.getPostviewOutputSurface()!=null&&surfaces.getPostviewOutputSurface().getSurface()!=null)
            throw new IllegalArgumentException("Postview not supported");
        return initSession(id,cameras,context,surfaces.getPreviewOutputSurface(),
            surfaces.getImageCaptureOutputSurface(),surfaces.getImageAnalysisOutputSurface());
    }
    public Camera2SessionConfigImpl initSession(String id,Map<String,CameraCharacteristics> cameras,
            Context context,OutputSurfaceImpl preview,OutputSurfaceImpl capture,OutputSurfaceImpl analysis) {
        synchronized(lock) {
            if(initialized)throw new IllegalStateException("Session processor already initialized");
            if(analysis!=null&&analysis.getSurface()!=null)throw new IllegalArgumentException("Analysis not supported");
            extender.init(id,cameras);
            if(!extender.isExtensionAvailable(id,cameras))throw new IllegalArgumentException("Unavailable extension");
            faceMapper=new OppoImageProcessor(extender.mode==OppoExtender.Mode.RETOUCH,extender.characteristics);
            Camera2SessionConfigImplBuilder config=new Camera2SessionConfigImplBuilder();
            try {
                if(extender.processed()||(capture!=null&&capture.getImageFormat()==ImageFormat.YUV_420_888)) {
                    releaseThread=new HandlerThread("OppoExtensionsRelease");releaseThread.start();
                }
                if(preview!=null&&preview.getSurface()!=null) {
                    previewSize=preview.getSize();
                    List<Size> allowed=extender.getSupportedPreviewOutputResolutions(id).get(preview.getImageFormat());
                    if(allowed==null||!allowed.contains(previewSize))throw new IllegalArgumentException("Unsupported preview");
                    // AUTO replaces its existing bounded preview output with
                    // YUV; it does not add an analysis stream to the HAL stream
                    // combination. Only use explicitly declared common sizes.
                    List<Size> yuv=extender.getSupportedPreviewOutputResolutions(id).get(ImageFormat.YUV_420_888);
                    boolean autoYuv=extender.mode==OppoExtender.Mode.AUTO&&yuv!=null&&yuv.contains(previewSize);
                    boolean nativeBokeh=extender.mode==OppoExtender.Mode.BOKEH&&extender.nativeMode()==7;
                    if(extender.mode==OppoExtender.Mode.RETOUCH||
                            (extender.mode==OppoExtender.Mode.BOKEH&&!nativeBokeh)||autoYuv) {
                        previewConfig=Camera2OutputConfigImplBuilder.newImageReaderConfig(previewSize,
                            ImageFormat.YUV_420_888,3,HardwareBuffer.USAGE_CPU_READ_OFTEN).build();
                        previewWriter=ImageWriter.newInstance(preview.getSurface(),2,ImageFormat.YUV_420_888);
                        previewWriter.setOnImageReleasedListener(writer->{ synchronized(lock) {
                            if(previewWriter==writer)previewOutputPending=false;
                        } },new Handler(releaseThread.getLooper()));
                    } else previewConfig=Camera2OutputConfigImplBuilder.newSurfaceConfig(preview.getSurface()).build();
                    config.addOutputConfig(previewConfig);
                }
                if(capture!=null&&capture.getSurface()!=null) {
                    outputSize=capture.getSize();
                    int format=capture.getImageFormat()==0x21?ImageFormat.JPEG:capture.getImageFormat();
                    List<Size> allowed=extender.getSupportedCaptureOutputResolutions(id).get(format);
                    if(allowed==null||!allowed.contains(outputSize))throw new IllegalArgumentException("Unsupported capture");
                    boolean nativeBokehYuv=format==ImageFormat.YUV_420_888&&
                        extender.mode==OppoExtender.Mode.BOKEH&&extender.nativeMode()==7;
                    nativeCaptureMode=format==ImageFormat.JPEG||nativeBokehYuv?extender.nativeMode():0;
                    // Keep the native rear beauty JPEG intact by default. An
                    // internal reader permits optional explicit edits without
                    // changing the native algorithm or its advertised sizes.
                    postprocessJpeg=format==ImageFormat.JPEG&&extender.mode==OppoExtender.Mode.RETOUCH&&
                        nativeCaptureMode!=0&&JpegBlobWriter.available();
                    if(postprocessJpeg||nativeBokehYuv||(nativeCaptureMode==0&&(extender.processed()||format==ImageFormat.YUV_420_888))) {
                        inputSize=postprocessJpeg||nativeBokehYuv?outputSize:(extender.processed()?extender.inputFor(outputSize):outputSize);
                        inputFormat=postprocessJpeg||nativeBokehYuv?ImageFormat.JPEG:
                            (extender.processed()?ImageFormat.YUV_420_888:ImageFormat.JPEG);
                        captureJpeg=format==ImageFormat.JPEG;
                        if(inputSize==null)throw new IllegalArgumentException("No physical input size");
                        captureConfig=Camera2OutputConfigImplBuilder.newImageReaderConfig(inputSize,
                            inputFormat,extender.processed()&&!nativeBokehYuv?4:2,HardwareBuffer.USAGE_CPU_READ_OFTEN).build();
                        ImageWriter.Builder writer=new ImageWriter.Builder(capture.getSurface()).setMaxImages(1).setImageFormat(format);
                        if(captureJpeg)writer.setWidthAndHeight(jpegCapacity(outputSize),1);
                        captureWriter=writer.build();
                        captureWriter.setOnImageReleasedListener(released->{ synchronized(lock) {
                            if(captureWriter==released)captureOutputPending=false;
                        } },new Handler(releaseThread.getLooper()));
                    } else captureConfig=Camera2OutputConfigImplBuilder.newSurfaceConfig(capture.getSurface()).build();
                    config.addOutputConfig(captureConfig);
                }
                if(previewConfig==null&&captureConfig==null)throw new IllegalArgumentException("No outputs");
                worker=Executors.newSingleThreadExecutor();timer=Executors.newSingleThreadScheduledExecutor();
                ++generation;closed.set(false);initialized=true;return config.build();
            } catch(RuntimeException failure) {
                closeWriters();stopReleaseThread();clearConfiguration();throw failure;
            }
        }
    }
    private static int jpegCapacity(Size size) {
        long bytes=(long)size.getWidth()*size.getHeight()*3+65536+8;
        if(bytes>Integer.MAX_VALUE)throw new IllegalArgumentException("JPEG too large");return (int)bytes;
    }
    private void closeWriters() {
        if(previewWriter!=null)previewWriter.close();previewWriter=null;
        if(captureWriter!=null)captureWriter.close();captureWriter=null;
        previewOutputPending=false;captureOutputPending=false;
    }
    private void stopReleaseThread() {
        if(releaseThread!=null)releaseThread.quitSafely();releaseThread=null;
    }
    private void clearConfiguration() {
        faceTracker=new FaceTracker();
        autoPolicy.reset();
        previewConfig=null;captureConfig=null;previewSize=null;inputSize=null;outputSize=null;
        faceMapper=null;captureJpeg=false;
        inputFormat=ImageFormat.YUV_420_888;nativeCaptureMode=0;postprocessJpeg=false;
    }
    private boolean active(long session) { return !closed.get()&&generation==session; }
    private boolean active(Burst current) {
        return active(current.generation)&&burst==current&&!current.terminal&&!current.cancelled.get();
    }
    public void onCaptureSessionStart(RequestProcessorImpl processor) {
        synchronized(lock) {
            if(!initialized||closed.get()||requests!=null||processor==null)
                throw new IllegalStateException("Session is not ready to start");
            requests=processor;final long session=generation;
            if(previewWriter!=null)requests.setImageProcessor(previewConfig.getId(),
                (stream,timestamp,reference,physical)->previewImage(session,stream,timestamp,reference,physical));
            if(captureWriter!=null)requests.setImageProcessor(captureConfig.getId(),
                (stream,timestamp,reference,physical)->captureImage(session,stream,timestamp,reference,physical));
        }
    }
    public void onCaptureSessionEnd() {
        closed.set(true);Burst current=burst;if(current!=null)current.cancelled.set(true);
        synchronized(lock) {
            ++generation;++repeatingGeneration;
            autoPolicy.reset();
            if(repeating!=null&&!repeating.terminal) {
                repeating.terminal=true;repeating.callback.onCaptureSequenceAborted(repeating.id);
            }
            repeating=null;
            cancelLocked(current);requests=null;previewImages.clear();previewResults.clear();closeWriters();
        }
    }
    public void deInitSession() {
        onCaptureSessionEnd();
        synchronized(lock) {
            if(worker!=null)worker.shutdownNow();if(timer!=null)timer.shutdownNow();
            worker=null;timer=null;initialized=false;burst=null;previewBusy=false;aborting=false;
            stopReleaseThread();
            parameters.clear();clearConfiguration();
        }
    }
    public void setParameters(Map<CaptureRequest.Key<?>,Object> supplied) {
        synchronized(lock) {
            // Validate before mutating any controls so a rejected batch cannot
            // leave a partially applied preview/capture parameter snapshot.
            List<CaptureRequest.Key> available=extender.getAvailableCaptureRequestKeys();
            if(OppoSurfaceControls.isAdvertised(available,CaptureRequest.EXTENSION_STRENGTH))
                validateStrength(OppoSurfaceControls.find(supplied,CaptureRequest.EXTENSION_STRENGTH));
            for(Map.Entry<CaptureRequest.Key<?>,Object> entry:supplied.entrySet())
                OppoSurfaceControls.validate(extender.characteristics,extender.mode.ordinal(),available,
                    entry.getKey(),entry.getValue());
            Map<CaptureRequest.Key<?>,Object> candidate=snapshot(parameters);
            for(Map.Entry<CaptureRequest.Key<?>,Object> entry:supplied.entrySet())
                if(OppoSurfaceControls.isAdvertised(available,entry.getKey())) {
                    if(entry.getValue()==null)removeByName(candidate,entry.getKey());
                    else putByName(candidate,entry.getKey(),copyValue(entry.getValue()));
                }
            validatePostprocess(candidate);
            OppoSurfaceControls.validateBatch(extender.characteristics,extender.mode.ordinal(),available,
                candidate,outputSize);
            boolean changed=false;
            for(CaptureRequest.Key<?> key:supplied.keySet())
                changed|=OppoSurfaceControls.isAdvertised(available,key);
            parameters.clear();parameters.putAll(candidate);
            // CameraX may update controls without calling startRepeating again.
            // Keep the app's sequence while replacing its underlying request.
            if(changed&&repeating!=null&&!repeating.terminal&&active(repeating.generation)&&requests!=null)
                submitPreviewLocked(repeating,Collections.emptyMap());
        }
    }
    /** The framework transports typed keys created by the client, so a control
     * is replaced by name rather than by key identity. */
    private static void putByName(Map<CaptureRequest.Key<?>,Object> values,CaptureRequest.Key<?> key,Object value) {
        removeByName(values,key);values.put(key,value);
    }
    private static void removeByName(Map<CaptureRequest.Key<?>,Object> values,CaptureRequest.Key<?> key) {
        values.keySet().removeIf(candidate->candidate!=null&&candidate.getName().equals(key.getName()));
    }
    private static void putIfAbsent(Map<CaptureRequest.Key<?>,Object> values,CaptureRequest.Key<?> key,Object value) {
        if(OppoSurfaceControls.find(values,key)==null)putByName(values,key,value);
    }
    private static Object copyValue(Object value) {
        if(value instanceof Rect)return new Rect((Rect)value);
        if(value instanceof MeteringRectangle[])return ((MeteringRectangle[])value).clone();
        if(value instanceof int[]) {
            if(((int[])value).length>1030)throw new IllegalArgumentException("Postprocess int payload too large");
            return ((int[])value).clone();
        }
        if(value instanceof float[]) {
            if(((float[])value).length>24)throw new IllegalArgumentException("Postprocess warp payload too large");
            return ((float[])value).clone();
        }
        if(value instanceof byte[]) {
            if(((byte[])value).length>32)throw new IllegalArgumentException("Postprocess text payload too large");
            return ((byte[])value).clone();
        }
        return value; // Other advertised controls have immutable scalar values.
    }
    private static Map<CaptureRequest.Key<?>,Object> snapshot(Map<CaptureRequest.Key<?>,Object> source) {
        Map<CaptureRequest.Key<?>,Object> copy=new HashMap<>();
        for(Map.Entry<CaptureRequest.Key<?>,Object> entry:source.entrySet())
            if(entry.getValue()!=null)copy.put(entry.getKey(),copyValue(entry.getValue()));
        return copy;
    }
    private Request request(int stage,int output,boolean still,boolean stabilize,
            Map<CaptureRequest.Key<?>,Object> app) {
        Map<CaptureRequest.Key<?>,Object> values=snapshot(app);
        values.remove(CaptureRequest.EXTENSION_STRENGTH); // Provider control, not a HAL control.
        OppoPostprocessControls.strip(values);
        OppoSurfaceControls.stripProvider(values);
        // Surface parameters drive standard Camera2 controls (tone map, color
        // correction) and the standard manual-exposure route. Explicit client
        // values for those standard keys are rejected by validateBatch instead
        // of being overwritten here.
        for(Map.Entry<CaptureRequest.Key<?>,Object> entry:OppoSurfaceControls.standard(extender.characteristics,
                extender.mode.ordinal(),app).entrySet())
            putIfAbsent(values,entry.getKey(),entry.getValue());
        for(Pair<CaptureRequest.Key,Object> pair:extender.stage(stage,still&&stabilize&&extender.processed()).getParameters())
            values.put(pair.first,pair.second);
        if(extender.mode==OppoExtender.Mode.RETOUCH&&extender.fullFaces()) {
            int[] face=OppoPostprocessControls.options(app).tracking;
            if(face[1]!=0||face[2]!=0||face[3]!=0||face[4]!=0)
                values.put(CaptureRequest.STATISTICS_FACE_DETECT_MODE,CaptureRequest.STATISTICS_FACE_DETECT_MODE_FULL);
        }
        // Internal locks are defaults. Explicit user locks remain orthogonal,
        // and manual sensor exposure must not inherit an automatic AE lock.
        for(CaptureRequest.Key<?> key:new CaptureRequest.Key<?>[]{CaptureRequest.CONTROL_AE_LOCK,CaptureRequest.CONTROL_AWB_LOCK})
            if(app.containsKey(key))values.put(key,app.get(key));
        if(Integer.valueOf(CaptureRequest.CONTROL_AE_MODE_OFF).equals(values.get(CaptureRequest.CONTROL_AE_MODE)))
            values.remove(CaptureRequest.CONTROL_AE_LOCK);
        if(stage>0) {
            values.put(CaptureRequest.CONTROL_AF_TRIGGER,CaptureRequest.CONTROL_AF_TRIGGER_IDLE);
            values.put(CaptureRequest.CONTROL_AE_PRECAPTURE_TRIGGER,CaptureRequest.CONTROL_AE_PRECAPTURE_TRIGGER_IDLE);
        }
        if(extender.nativeControlsAvailable()) {
            // Reset even on preview/software requests: a previous native
            // still must not leave a sticky algorithm in the backend.
            int nativeMode=still?nativeModeFor(app):0;
            values.put(OppoAdvancedExtender.NATIVE_STILL_MODE,nativeMode);
            values.put(OppoAdvancedExtender.NATIVE_BEAUTY_STRENGTH,
                nativeMode==5||nativeMode==7?strength(app):0);
            if(extender.supportsNativeMode(9)) {
                Object ae=app.get(CaptureRequest.CONTROL_AE_MODE),flash=app.get(CaptureRequest.FLASH_MODE);
                boolean automatic=(ae==null||Integer.valueOf(CaptureRequest.CONTROL_AE_MODE_ON).equals(ae))&&
                    (flash==null||Integer.valueOf(CaptureRequest.FLASH_MODE_OFF).equals(flash));
                // The native classifier consumes the completed scene/fused
                // NV21. It supplements exposure/motion selection with actual
                // semantic inference; preview never inherits a still setting.
                values.put(OppoAdvancedExtender.NATIVE_SCENE_STRENGTH,
                    still&&automatic&&extender.mode==OppoExtender.Mode.AUTO&&
                    nativeCaptureMode!=0&&strength(app)>0?35:0);
            }
            if(still&&nativeCaptureMode!=0&&nativeMode==0) {
                Integer noise=nonBurstNoiseReduction();
                if(noise!=null)values.put(CaptureRequest.NOISE_REDUCTION_MODE,noise);
            }
        }
        if(nativeCaptureMode!=0) {
            // Native JPEG capture owns its burst and scene selection. Preview
            // stays in AUTO, and manual/flash requests become plain HAL JPEG.
            values.put(CaptureRequest.CONTROL_MODE,CaptureRequest.CONTROL_MODE_AUTO);
            values.remove(CaptureRequest.CONTROL_SCENE_MODE);
        }
        // DOC keeps its documented standard surface (SCENE_MODE_BARCODE) even
        // when a native document stage owns the pixels.
        for(Map.Entry<CaptureRequest.Key<?>,Object> entry:OppoSurfaceControls.scene(extender.characteristics,
                extender.mode.ordinal(),app).entrySet())
            values.put(entry.getKey(),entry.getValue());
        // A YUV caller expects unrotated sensor geometry. Decode an unrotated
        // internal scene-processed JPEG; return the caller's JPEG controls in
        // the result separately, as required by Extensions.
        if(still&&inputFormat==ImageFormat.JPEG&&captureWriter!=null&&(!captureJpeg||postprocessActive(app)))
            values.put(CaptureRequest.JPEG_ORIENTATION,0);
        return new Request(stage,output,OppoSurfaceControls.template(still,app),
            Collections.unmodifiableMap(values));
    }
    @SuppressWarnings("unchecked")
    private Map<CaptureResult.Key,Object> resultValues(TotalCaptureResult result,Map<CaptureRequest.Key<?>,Object> settings) {
        Map<CaptureResult.Key,Object> values=new HashMap<>();
        for(CaptureResult.Key key:extender.getAvailableCaptureResultKeys()) {
            Object value=result.get(key);if(value!=null)values.put(key,value);
        }
        if(extender.getAvailableCaptureResultKeys().contains(CaptureResult.EXTENSION_STRENGTH))
            values.put(CaptureResult.EXTENSION_STRENGTH,strength(settings));
        // Every advertised surface request key has a same-name result that
        // carries the submitted snapshot when the HAL does not publish its own
        // value. The HAL's actual value always wins.
        OppoSurfaceControls.snapshotResults(values,extender.getAvailableCaptureResultKeys(),settings);
        if(extender.mode==OppoExtender.Mode.RETOUCH) {
            OppoPostprocessControls.results(values,settings);
            if(!extender.fullFaces())values.remove(OppoPostprocessControls.FACE_TRACKING_RESULT);
        }
        return values;
    }
    private int strength(Map<CaptureRequest.Key<?>,Object> settings) {
        Object value=settings.get(CaptureRequest.EXTENSION_STRENGTH);
        return value instanceof Integer?(Integer)value:
            (extender.mode==OppoExtender.Mode.RETOUCH||extender.mode==OppoExtender.Mode.BOKEH?50:100);
    }
    private static void validateStrength(Object value) {
        if(value!=null&&(!(value instanceof Integer)||(Integer)value<0||(Integer)value>100))
            throw new IllegalArgumentException("Extension strength must be between 0 and 100");
    }
    private boolean postprocessActive(Map<CaptureRequest.Key<?>,Object> settings) {
        return extender.mode==OppoExtender.Mode.RETOUCH&&strength(settings)>0&&OppoPostprocessControls.options(settings).active();
    }
    private void validatePostprocess(Map<CaptureRequest.Key<?>,Object> settings) {
        if(extender.mode!=OppoExtender.Mode.RETOUCH)return;
        PortraitPostprocess.Options options=OppoPostprocessControls.options(settings);
        if(options.active()&&strength(settings)>0&&outputSize!=null&&
                ((long)outputSize.getWidth()*outputSize.getHeight()>PixelPipeline.MAX_PIXELS||
                (nativeCaptureMode!=0&&captureJpeg&&!postprocessJpeg)))
            throw new IllegalArgumentException("Explicit photo edits require a JPEG writer and output at most 4 MiPixels");
    }
    private abstract static class Callback implements RequestProcessorImpl.Callback {
        public void onCaptureStarted(RequestProcessorImpl.Request request,long frame,long timestamp) {}
        public void onCaptureProgressed(RequestProcessorImpl.Request request,CaptureResult result) {}
        public void onCaptureCompleted(RequestProcessorImpl.Request request,TotalCaptureResult result) {}
        public void onCaptureFailed(RequestProcessorImpl.Request request,CaptureFailure failure) {}
        public void onCaptureBufferLost(RequestProcessorImpl.Request request,long frame,int stream) {}
        public void onCaptureSequenceCompleted(int sequence,long frame) {}
        public void onCaptureSequenceAborted(int sequence) {}
    }
    private int previewRequest(CaptureCallback callback,Map<CaptureRequest.Key<?>,Object> additions,boolean repeating) {
        final int id=nextId.getAndIncrement();
        synchronized(lock) {
            if(closed.get()||requests==null||previewConfig==null) {
                callback.onCaptureFailed(id);callback.onCaptureSequenceCompleted(id);return id;
            }
            PreviewSequence sequence=new PreviewSequence(id,generation,callback,repeating);
            if(repeating)this.repeating=sequence;
            submitPreviewLocked(sequence,additions);return id;
        }
    }
    private boolean previewActive(PreviewSequence sequence,long revision,long repeat) {
        return active(sequence.generation)&&!sequence.terminal&&sequence.revision==revision&&
            (!sequence.repeating||(repeating==sequence&&repeat==repeatingGeneration));
    }
    private void submitPreviewLocked(PreviewSequence sequence,Map<CaptureRequest.Key<?>,Object> additions) {
        final int id=sequence.id;final CaptureCallback callback=sequence.callback;
        final long revision=++sequence.revision;
        final long repeat=sequence.repeating?++repeatingGeneration:repeatingGeneration;
        final AtomicBoolean submissionNotified=new AtomicBoolean();
        if(sequence.repeating) { autoPolicy.reset();previewImages.clear();previewResults.clear(); }
        Map<CaptureRequest.Key<?>,Object> values=snapshot(parameters);
        for(Map.Entry<CaptureRequest.Key<?>,Object> entry:additions.entrySet()) {
            if(entry.getValue()==null)values.remove(entry.getKey());
            else values.put(entry.getKey(),copyValue(entry.getValue()));
        }
        validatePostprocess(values);
        // Triggers are submitted once by startTrigger. Retaining START in
        // setParameters must not restart AF/AE for each repeating frame.
        if(sequence.repeating) {
            values.put(CaptureRequest.CONTROL_AF_TRIGGER,CaptureRequest.CONTROL_AF_TRIGGER_IDLE);
            values.put(CaptureRequest.CONTROL_AE_PRECAPTURE_TRIGGER,CaptureRequest.CONTROL_AE_PRECAPTURE_TRIGGER_IDLE);
        }
        Request request=request(0,previewConfig.getId(),false,false,values);
        Callback listener=new Callback() {
            public void onCaptureStarted(RequestProcessorImpl.Request r,long f,long timestamp) {
                synchronized(lock) {
                    if(r==request&&previewActive(sequence,revision,repeat)&&!sequence.failed)
                        callback.onCaptureStarted(id,timestamp);
                }
            }
            public void onCaptureCompleted(RequestProcessorImpl.Request r,TotalCaptureResult result) {
                synchronized(lock) {
                    if(r!=request||!previewActive(sequence,revision,repeat)||sequence.failed||
                            (!sequence.repeating&&sequence.notified))return;
                    Long timestamp=result.get(CaptureResult.SENSOR_TIMESTAMP);
                    if(timestamp==null) {
                        if(sequence.repeating)autoPolicy.reset();
                        if(!sequence.repeating)sequence.failed=true;
                        sequence.notified=true;submissionNotified.set(true);callback.onCaptureFailed(id);return;
                    }
                    boolean automatic=sequence.repeating&&automaticPreview(result);
                    if(sequence.repeating&&extender.mode==OppoExtender.Mode.AUTO&&!automatic)autoPolicy.reset();
                    if(previewWriter!=null) {
                        previewResults.put(timestamp,new PreviewResult(result,System.nanoTime(),automatic,strength(values),
                            extender.mode==OppoExtender.Mode.RETOUCH?OppoPostprocessControls.options(values):null));
                        trim(previewResults,3);pairPreview(timestamp);
                    }
                    sequence.notified=true;submissionNotified.set(true);
                    callback.onCaptureProcessStarted(id);
                    if(!previewActive(sequence,revision,repeat)||sequence.failed)return;
                    callback.onCaptureCompleted(timestamp,id,resultValues(result,values));
                }
            }
            public void onCaptureFailed(RequestProcessorImpl.Request r,CaptureFailure failure) {
                synchronized(lock) {
                    if(r==request&&previewActive(sequence,revision,repeat)&&!sequence.failed&&
                            (sequence.repeating||!sequence.notified)) {
                        if(sequence.repeating)autoPolicy.reset();
                        if(!sequence.repeating)sequence.failed=true;
                        sequence.notified=true;submissionNotified.set(true);
                        callback.onCaptureFailed(id);
                    }
                }
            }
            public void onCaptureBufferLost(RequestProcessorImpl.Request r,long frame,int stream) {
                if(stream==request.output)onCaptureFailed(r,null);
            }
            public void onCaptureSequenceCompleted(int sequence,long frame) {
                synchronized(lock) { finishPreviewLocked(revision,false); }
            }
            public void onCaptureSequenceAborted(int sequence) {
                synchronized(lock) { finishPreviewLocked(revision,true); }
            }
            private void finishPreviewLocked(long completedRevision,boolean aborted) {
                // setParameters retires the previous hardware sequence,
                // but it must not terminate the app's replacement sequence.
                if(!active(sequence.generation)||sequence.terminal||sequence.revision!=completedRevision)return;
                sequence.terminal=true;
                if(sequence.repeating)autoPolicy.reset();
                if(OppoSessionProcessor.this.repeating==sequence)OppoSessionProcessor.this.repeating=null;
                if(!aborted&&!sequence.notified) {
                    sequence.failed=true;sequence.notified=true;callback.onCaptureFailed(id);
                }
                if(aborted)callback.onCaptureSequenceAborted(id);else callback.onCaptureSequenceCompleted(id);
            }
        };
        try {
            int submitted=sequence.repeating?requests.setRepeating(request,listener):requests.submit(request,listener);
            if(submitted<0&&previewActive(sequence,revision,repeat)) {
                sequence.terminal=true;if(repeating==sequence)repeating=null;
                if(!submissionNotified.get())callback.onCaptureFailed(id);
                callback.onCaptureSequenceCompleted(id);
            }
        } catch(RuntimeException failure) {
            if(previewActive(sequence,revision,repeat)) {
                sequence.terminal=true;if(repeating==sequence)repeating=null;
                if(!submissionNotified.get())callback.onCaptureFailed(id);
                callback.onCaptureSequenceCompleted(id);
            }
        }
    }
    public int startRepeating(CaptureCallback callback) { return previewRequest(callback,Collections.emptyMap(),true); }
    public int startTrigger(Map<CaptureRequest.Key<?>,Object> triggers,CaptureCallback callback) {
        Map<CaptureRequest.Key<?>,Object> allowed=new HashMap<>();
        List<CaptureRequest.Key> available=extender.getAvailableCaptureRequestKeys();
        for(Map.Entry<CaptureRequest.Key<?>,Object> item:triggers.entrySet()) {
            // A known surface control is never silently dropped, even when this
            // camera or mode cannot provide it.
            OppoSurfaceControls.validate(extender.characteristics,extender.mode.ordinal(),available,
                item.getKey(),item.getValue());
            if(!OppoSurfaceControls.isAdvertised(available,item.getKey()))continue;
            if(item.getKey().getName().equals(CaptureRequest.EXTENSION_STRENGTH.getName()))
                validateStrength(item.getValue());
            allowed.put(item.getKey(),item.getValue());
        }
        synchronized(lock) {
            Map<CaptureRequest.Key<?>,Object> candidate=snapshot(parameters);
            for(Map.Entry<CaptureRequest.Key<?>,Object> entry:allowed.entrySet())
                putByName(candidate,entry.getKey(),copyValue(entry.getValue()));
            OppoSurfaceControls.validateBatch(extender.characteristics,extender.mode.ordinal(),available,
                candidate,outputSize);
        }
        return previewRequest(callback,allowed,false);
    }
    public void stopRepeating() { synchronized(lock) {
        autoPolicy.reset();
        ++repeatingGeneration;repeating=null;previewImages.clear();previewResults.clear();
        if(requests!=null)requests.stopRepeating();
    } }
    public int startCaptureWithPostview(CaptureCallback callback) { throw new UnsupportedOperationException("No postview"); }
    private static boolean flashRequested(Map<CaptureRequest.Key<?>,Object> settings) {
        Object ae=settings.get(CaptureRequest.CONTROL_AE_MODE),flash=settings.get(CaptureRequest.FLASH_MODE);
        return Integer.valueOf(CaptureRequest.CONTROL_AE_MODE_ON_AUTO_FLASH).equals(ae)||
            Integer.valueOf(CaptureRequest.CONTROL_AE_MODE_ON_ALWAYS_FLASH).equals(ae)||
            Integer.valueOf(CaptureRequest.CONTROL_AE_MODE_ON_AUTO_FLASH_REDEYE).equals(ae)||
            Integer.valueOf(CaptureRequest.FLASH_MODE_SINGLE).equals(flash)||
            Integer.valueOf(CaptureRequest.FLASH_MODE_TORCH).equals(flash);
    }
    private static boolean automaticPreview(TotalCaptureResult result) {
        Integer flashState=result.get(CaptureResult.FLASH_STATE);
        return Integer.valueOf(CaptureRequest.CONTROL_AE_MODE_ON).equals(result.get(CaptureResult.CONTROL_AE_MODE))&&
            Integer.valueOf(CaptureResult.CONTROL_AE_STATE_CONVERGED).equals(result.get(CaptureResult.CONTROL_AE_STATE))&&
            Integer.valueOf(CaptureRequest.FLASH_MODE_OFF).equals(result.get(CaptureResult.FLASH_MODE))&&
            !Integer.valueOf(CaptureResult.FLASH_STATE_FIRED).equals(flashState)&&
            !Integer.valueOf(CaptureResult.FLASH_STATE_PARTIAL).equals(flashState);
    }
    private int nativeModeFor(Map<CaptureRequest.Key<?>,Object> settings) {
        if(strength(settings)==0)return 0;
        // An explicit, validated application selection owns the still stage.
        // The client asked for that stage; the automatic capture policy is not
        // consulted. Manual exposure stages (17/18) are also selected this way.
        int requested=OppoSurfaceControls.requestedStillMode(settings);
        if(requested!=0)return requested;
        Object ae=settings.get(CaptureRequest.CONTROL_AE_MODE),flash=settings.get(CaptureRequest.FLASH_MODE);
        if((ae!=null&&!Integer.valueOf(CaptureRequest.CONTROL_AE_MODE_ON).equals(ae))||
                (flash!=null&&!Integer.valueOf(CaptureRequest.FLASH_MODE_OFF).equals(flash)))return 0;
        if(nativeCaptureMode!=0&&extender.mode==OppoExtender.Mode.AUTO) {
            int selected=autoPolicy.select(System.nanoTime(),extender.supportsNativeMode(1),
                extender.supportsNativeMode(2),extender.supportsNativeMode(3),extender.supportsNativeMode(4));
            if(selected==0&&nativeCaptureMode==9)return 9; // Single-frame semantic fallback.
            // The HAL's standard HIGH_QUALITY NR path can schedule MFNR even
            // with native mode 0. Only claim a plain-motion bypass when FAST
            // or OFF can actually prevent that implicit burst.
            if(selected==0&&nonBurstNoiseReduction()==null)
                return extender.supportsNativeMode(3)?3:nativeCaptureMode;
            return selected;
        }
        return nativeCaptureMode;
    }
    private Integer nonBurstNoiseReduction() {
        int[] modes=extender.characteristics.get(CameraCharacteristics.NOISE_REDUCTION_AVAILABLE_NOISE_REDUCTION_MODES);
        if(OppoExtender.contains(modes,CaptureRequest.NOISE_REDUCTION_MODE_FAST))return CaptureRequest.NOISE_REDUCTION_MODE_FAST;
        if(OppoExtender.contains(modes,CaptureRequest.NOISE_REDUCTION_MODE_OFF))return CaptureRequest.NOISE_REDUCTION_MODE_OFF;
        return null;
    }
    public int startCapture(CaptureCallback callback) {
        int id=nextId.getAndIncrement();
        synchronized(lock) {
            if(closed.get()||requests==null||captureConfig==null||burst!=null||aborting||captureOutputPending) {
                callback.onCaptureFailed(id);callback.onCaptureSequenceCompleted(id);return id;
            }
            final Map<CaptureRequest.Key<?>,Object> settings=snapshot(parameters);
            validatePostprocess(settings);
            final boolean expression=extender.mode==OppoExtender.Mode.RETOUCH&&
                OppoPostprocessControls.options(settings).tracking[4]!=0;
            final Burst current=new Burst(id,captureWriter!=null&&(nativeCaptureMode==0||expression)&&
                extender.processed()&&strength(settings)>0&&!flashRequested(settings)?4:1,
                generation,callback,settings);burst=current;
            List<RequestProcessorImpl.Request> captures=new ArrayList<>();
            for(int i=0;i<current.count;++i)captures.add(request(i,captureConfig.getId(),true,current.count>1,current.parameters));
            current.timeout=timer.schedule(()->abortCapture(id),45,TimeUnit.SECONDS);
            Callback listener=new Callback() {
                public void onCaptureStarted(RequestProcessorImpl.Request r,long frame,long timestamp) {
                    synchronized(lock) {
                        if(!active(current)||!captures.contains(r))return;
                        int stage=((Request)r).stage;
                        if(current.started[stage]>=0)return;
                        current.started[stage]=timestamp;
                        if(stage==0)callback.onCaptureStarted(id,timestamp);
                    }
                }
                public void onCaptureCompleted(RequestProcessorImpl.Request r,TotalCaptureResult result) {
                    synchronized(lock) {
                        if(!active(current)||!captures.contains(r))return;
                        Long timestamp=result.get(CaptureResult.SENSOR_TIMESTAMP);
                        if(timestamp==null) { failLocked(current);return; }
                        int stage=((Request)r).stage;
                        if(current.started[stage]>=0&&current.started[stage]!=timestamp) { failLocked(current);return; }
                        for(int i=0;i<current.count;++i)if(current.results[i]!=null) {
                            Long previous=current.results[i].get(CaptureResult.SENSOR_TIMESTAMP);
                            if(i==stage) { if(previous.equals(timestamp))return;failLocked(current);return; }
                            if(previous.equals(timestamp)) { failLocked(current);return; }
                        }
                        current.results[stage]=result;
                        if(captureWriter!=null)pairCapture(current);
                        else finishDirectCaptureLocked(current);
                    }
                }
                public void onCaptureFailed(RequestProcessorImpl.Request r,CaptureFailure failure) {
                    synchronized(lock) { if(captures.contains(r))failLocked(current); }
                }
                public void onCaptureBufferLost(RequestProcessorImpl.Request r,long frame,int stream) {
                    synchronized(lock) { if(captures.contains(r)&&stream==r.getTargetOutputConfigIds().get(0))failLocked(current); }
                }
                public void onCaptureSequenceAborted(int sequence) { synchronized(lock) { cancelLocked(current); } }
                public void onCaptureSequenceCompleted(int sequence,long frame) {
                    synchronized(lock) {
                        if(captureWriter==null&&burst==current&&!current.terminal) {
                            current.sequenceCompleted=true;
                            finishDirectCaptureLocked(current);
                        }
                    }
                }
            };
            try { if(requests.submit(captures,listener)<0)failLocked(current); }
            catch(RuntimeException failure) { failLocked(current); }
            return id;
        }
    }
    private void finishDirectCaptureLocked(Burst current) {
        if(!active(current)||!current.sequenceCompleted||current.results[0]==null)return;
        // Direct JPEG results alone do not prove delivery. Wait for the
        // hardware sequence to finish so a late buffer loss cannot follow
        // an already reported successful still transaction.
        current.processStarted=true;current.callback.onCaptureProcessStarted(current.id);
        if(!active(current))return;
        TotalCaptureResult result=current.results[0];
        long timestamp=result.get(CaptureResult.SENSOR_TIMESTAMP);
        completeLocked(current);
        current.callback.onCaptureCompleted(timestamp,current.id,resultValues(result,current.parameters));
        current.callback.onCaptureSequenceCompleted(current.id);
    }
    private void completeLocked(Burst current) {
        current.terminal=true;if(current.timeout!=null)current.timeout.cancel(false);
        current.images.clear();if(!current.processing)burst=null;
    }
    private void failLocked(Burst current) {
        if(current==null||burst!=current||current.terminal)return;
        current.cancelled.set(true);completeLocked(current);current.callback.onCaptureFailed(current.id);
        current.callback.onCaptureSequenceCompleted(current.id);
    }
    private void cancelLocked(Burst current) {
        if(current==null||burst!=current||current.terminal)return;
        current.cancelled.set(true);completeLocked(current);current.callback.onCaptureSequenceAborted(current.id);
    }
    public void abortCapture(int id) {
        Burst current=burst;if(current==null||current.id!=id||current.terminal)return;
        current.cancelled.set(true);
        synchronized(lock) {
            if(burst!=current||current.terminal)return;
            aborting=true;
            try { cancelLocked(current);if(requests!=null)requests.abortCaptures(); }
            finally { aborting=false; }
        }
    }
    public Pair<Long,Long> getRealtimeCaptureLatency() { return null; }
    private static <T> void trim(LinkedHashMap<Long,T> values,int maximum) {
        while(values.size()>maximum)values.remove(values.keySet().iterator().next());
    }
    private void captureImage(long session,int stream,long timestamp,ImageReferenceImpl reference,String physicalId) {
        try {
            Image image=reference.get();if(image==null)return;
            byte[] copy;
            synchronized(lock) {
                if(!active(session)||burst==null||burst.terminal||burst.processing||stream!=captureConfig.getId())return;
                if(image.getTimestamp()!=timestamp||image.getWidth()!=inputSize.getWidth()||image.getHeight()!=inputSize.getHeight()) {
                    return;
                }
                if(image.getFormat()!=inputFormat)return;
                if(inputFormat==ImageFormat.JPEG) {
                    if(image.getPlanes().length!=1)return;
                    ByteBuffer bytes=image.getPlanes()[0].getBuffer().duplicate();
                    if(bytes.remaining()<4||bytes.remaining()>jpegCapacity(inputSize))return;
                    copy=new byte[bytes.remaining()];bytes.get(copy);
                } else copy=OppoImageProcessor.read(image);
                burst.images.put(timestamp,copy);trim(burst.images,8);pairCapture(burst);
            }
        } catch(RuntimeException failure) { /* Invalid or late input cannot fail a newer burst. */ }
        finally { reference.decrement(); }
    }
    private void pairCapture(Burst current) {
        if(current.processing||current.terminal||current.cancelled.get())return;
        byte[][] frames=new byte[current.count][];TotalCaptureResult[] metadata=new TotalCaptureResult[current.count];
        for(int i=0;i<current.count;++i) {
            metadata[i]=current.results[i];
            if(metadata[i]==null)return;
            Long timestamp=metadata[i].get(CaptureResult.SENSOR_TIMESTAMP);frames[i]=current.images.get(timestamp);
            if(frames[i]==null)return;
        }
        current.processing=true;current.processStarted=true;current.images.clear();
        current.callback.onCaptureProcessStarted(current.id);
        if(!active(current)) { current.processing=false;if(burst==current)burst=null;return; }
        final Size input=inputSize,output=outputSize;final boolean jpeg=captureJpeg,decode=inputFormat==ImageFormat.JPEG;
        final OppoImageProcessor mapper=faceMapper;
        try { worker.execute(()->processCapture(current,frames,metadata,input,output,jpeg,decode,mapper)); }
        catch(RuntimeException failure) { current.processing=false;failLocked(current); }
    }
    private void processCapture(Burst current,byte[][] frames,TotalCaptureResult[] metadata,
            Size inputSize,Size outputSize,boolean captureJpeg,boolean decode,OppoImageProcessor faceMapper) {
        try {
            int w=inputSize.getWidth(),h=inputSize.getHeight();boolean[] eligible=new boolean[frames.length];
            PixelPipeline.Face[][] faces=new PixelPipeline.Face[frames.length][];
            for(int i=0;i<frames.length;++i) {
                eligible[i]=i==0||OppoImageProcessor.sameExposure(metadata[0],metadata[i]);faces[i]=faceMapper.faces(metadata[i],w,h);
            }
            boolean edits=postprocessActive(current.parameters);
            boolean copyJpeg=decode&&captureJpeg&&!edits;
            byte[] output=null;
            if(copyJpeg)PixelPipeline.checkCancelled(()->!active(current.generation)||current.cancelled.get());
            else if(decode)output=JpegYuvDecoder.decode(frames[0],w,h,()->!active(current.generation)||current.cancelled.get());
            else if(!inputSize.equals(outputSize))output=SuperResolution.reconstruct(frames,w,h,eligible,
                ()->!active(current.generation)||current.cancelled.get()).nv21;
            else output=PixelPipeline.process(frames,w,h,faces,extender.mode==OppoExtender.Mode.RETOUCH,true,eligible,strength(current.parameters),
                ()->!active(current.generation)||current.cancelled.get());
            if(extender.mode==OppoExtender.Mode.BOKEH&&!decode)
                output=PixelPipeline.bokeh(output,w,h,faces[0],strength(current.parameters),
                    ()->!active(current.generation)||current.cancelled.get());
            if(edits&&OppoPostprocessControls.options(current.parameters).tracking[4]!=0) {
                byte[][] expressions=frames;
                if(decode) {
                    expressions=new byte[frames.length][];expressions[0]=output.clone();
                    for(int i=1;i<frames.length;++i)expressions[i]=JpegYuvDecoder.decode(frames[i],w,h,
                        ()->!active(current.generation)||current.cancelled.get());
                }
                output=FaceExpression.apply(output,expressions,w,h,faces,eligible,strength(current.parameters),
                    ()->!active(current.generation)||current.cancelled.get());
            }
            if(edits)output=PortraitPostprocess.process(output,outputSize.getWidth(),outputSize.getHeight(),
                faces[0],OppoPostprocessControls.options(current.parameters),strength(current.parameters),
                ()->!active(current.generation)||current.cancelled.get());
            byte[] encoded=captureJpeg?(copyJpeg?frames[0]:encodeJpeg(output,outputSize,current.parameters,metadata[0])):null;
            synchronized(lock) {
                if(!active(current))return;
                long timestamp=metadata[0].get(CaptureResult.SENSOR_TIMESTAMP);
                Image image=captureWriter.dequeueInputImage();boolean queued=false;
                try {
                    if(captureJpeg)writeJpeg(encoded,image,jpegCapacity(outputSize));
                    else OppoImageProcessor.write(output,image,outputSize.getWidth(),outputSize.getHeight());
                    image.setTimestamp(timestamp);
                    if(closed.get()||current.cancelled.get())return;
                    captureOutputPending=true;
                    try { captureWriter.queueInputImage(image);queued=true; }
                    catch(RuntimeException failure) { captureOutputPending=false;throw failure; }
                } finally { if(!queued)image.close(); }
                completeLocked(current);
                Map<CaptureResult.Key,Object> values=resultValues(metadata[0],current.parameters);
                if(captureJpeg||decode) {
                    values.put(CaptureResult.JPEG_QUALITY,(byte)jpegQuality(current.parameters,metadata[0]));
                    values.put(CaptureResult.JPEG_ORIENTATION,jpegOrientation(current.parameters,metadata[0]));
                }
                current.callback.onCaptureCompleted(timestamp,current.id,values);
                current.callback.onCaptureSequenceCompleted(current.id);
            }
        } catch(CancellationException ignored) { synchronized(lock) { cancelLocked(current); } }
        catch(RuntimeException|OutOfMemoryError failure) { synchronized(lock) { failLocked(current); } }
        finally { synchronized(lock) { current.processing=false;if(burst==current&&current.terminal)burst=null; } }
    }
    static void writeJpeg(byte[] jpeg,Image image,int capacity) {
        if(image.getFormat()!=ImageFormat.JPEG||image.getWidth()!=capacity||image.getHeight()!=1||
                image.getPlanes().length!=1)
            throw new IllegalArgumentException("Unexpected JPEG writer allocation");
        JpegBlobWriter.write(jpeg,image,capacity);
    }
    private static int jpegQuality(Map<CaptureRequest.Key<?>,Object> settings,TotalCaptureResult anchor) {
        Object value=settings.get(CaptureRequest.JPEG_QUALITY);
        if(value==null)value=anchor.get(CaptureResult.JPEG_QUALITY);
        return value instanceof Byte?((Byte)value)&255:95;
    }
    private static int jpegOrientation(Map<CaptureRequest.Key<?>,Object> settings,TotalCaptureResult anchor) {
        Object value=settings.get(CaptureRequest.JPEG_ORIENTATION);
        if(value==null)value=anchor.get(CaptureResult.JPEG_ORIENTATION);
        return value instanceof Integer?(Integer)value:0;
    }
    private byte[] encodeJpeg(byte[] pixels,Size outputSize,Map<CaptureRequest.Key<?>,Object> settings,TotalCaptureResult anchor) {
        int quality=jpegQuality(settings,anchor),orientation=jpegOrientation(settings,anchor);
        if(quality<1||quality>100||orientation%90!=0)throw new IllegalArgumentException("Invalid JPEG controls");
        final int capacity=jpegCapacity(outputSize)-44;
        ByteArrayOutputStream output=new ByteArrayOutputStream(Math.min(capacity,1024*1024)) {
            public synchronized void write(byte[] bytes,int offset,int size) {
                if(size>capacity-count)throw new IllegalArgumentException("JPEG exceeds size budget");super.write(bytes,offset,size);
            }
            public synchronized void write(int value) {
                if(count==capacity)throw new IllegalArgumentException("JPEG exceeds size budget");super.write(value);
            }
        };
        YuvImage image=new YuvImage(pixels,ImageFormat.NV21,outputSize.getWidth(),outputSize.getHeight(),null);
        if(!image.compressToJpeg(new Rect(0,0,outputSize.getWidth(),outputSize.getHeight()),quality,output))
            throw new IllegalStateException("JPEG encoding failed");
        return JpegBlob.withOrientation(output.toByteArray(),orientation,OppoSurfaceControls.jpegComment(settings));
    }
    private void previewImage(long session,int stream,long timestamp,ImageReferenceImpl reference,String physicalId) {
        try {
            Image image=reference.get();if(image==null)return;
            synchronized(lock) {
                if(!active(session)||previewWriter==null||stream!=previewConfig.getId()||previewBusy||previewOutputPending||burst!=null)return;
                if(image.getTimestamp()!=timestamp||image.getWidth()!=previewSize.getWidth()||image.getHeight()!=previewSize.getHeight())return;
                previewImages.put(timestamp,new PreviewImage(OppoImageProcessor.read(image),System.nanoTime()));
                trim(previewImages,3);pairPreview(timestamp);
            }
        } catch(RuntimeException|OutOfMemoryError ignored) { /* Drop invalid preview input; always release its reference. */ }
        finally { reference.decrement(); }
    }
    private void pairPreview(long timestamp) {
        PreviewImage frame=previewImages.get(timestamp);PreviewResult metadata=previewResults.get(timestamp);
        if(frame==null||metadata==null)return;
        previewImages.remove(timestamp);previewResults.remove(timestamp);
        if(previewBusy||previewOutputPending||burst!=null||closed.get())return;
        final byte[] pixels=frame.pixels;final TotalCaptureResult result=metadata.metadata;
        if(extender.mode==OppoExtender.Mode.AUTO) {
            Long exposure=result.get(CaptureResult.SENSOR_EXPOSURE_TIME);
            Integer iso=result.get(CaptureResult.SENSOR_SENSITIVITY);
            autoPolicy.update(timestamp,exposure==null?0:exposure,iso==null?0:iso,metadata.automatic,
                pixels,previewSize.getWidth(),previewSize.getHeight(),frame.arrival,metadata.arrival,System.nanoTime());
        }
        previewBusy=true;
        final long session=generation,repeat=repeatingGeneration;
        final Size size=previewSize;final OppoImageProcessor mapper=faceMapper;final FaceTracker tracker=faceTracker;
        final int previewStrength=metadata.strength;
        final PortraitPostprocess.Options previewEdits=metadata.postprocess;
        try { worker.execute(()-> {
            try {
                int w=size.getWidth(),h=size.getHeight();
                byte[] output=pixels;
                if(extender.mode!=OppoExtender.Mode.AUTO) {
                    PixelPipeline.Face[] faces=mapper.faces(result,w,h);
                    if(extender.mode==OppoExtender.Mode.RETOUCH)
                        faces=tracker.update(faces,w,h,timestamp);
                    output=PixelPipeline.process(new byte[][]{pixels},w,h,
                        new PixelPipeline.Face[][]{faces},extender.mode==OppoExtender.Mode.RETOUCH,false,null,previewStrength,
                        ()->!active(session)||repeat!=repeatingGeneration);
                    if(extender.mode==OppoExtender.Mode.BOKEH)
                        output=PixelPipeline.bokeh(output,w,h,faces,previewStrength,()->!active(session)||repeat!=repeatingGeneration);
                    if(previewEdits!=null&&previewEdits.active())
                        output=PortraitPostprocess.process(output,w,h,faces,previewEdits,previewStrength,
                            ()->!active(session)||repeat!=repeatingGeneration);
                }
                synchronized(lock) {
                    if(!active(session)||repeat!=repeatingGeneration||previewWriter==null)return;
                    Image image=previewWriter.dequeueInputImage();boolean queued=false;
                    try {
                        OppoImageProcessor.write(output,image,w,h);image.setTimestamp(timestamp);
                        if(closed.get())return;
                        previewOutputPending=true;
                        try { previewWriter.queueInputImage(image);queued=true; }
                        catch(RuntimeException failure) { previewOutputPending=false;throw failure; }
                    } finally { if(!queued)image.close(); }
                }
            } catch(RuntimeException|OutOfMemoryError ignored) { /* Drop this preview frame; no retained input. */ }
            finally { synchronized(lock) { if(generation==session)previewBusy=false; } }
        }); } catch(RuntimeException failure) { previewBusy=false; }
    }
}
