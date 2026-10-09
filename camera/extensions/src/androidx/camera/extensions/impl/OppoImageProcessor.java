/* SPDX-License-Identifier: BSD-3-Clause */
package androidx.camera.extensions.impl;

import android.graphics.ImageFormat;
import android.graphics.Rect;
import android.graphics.Point;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CaptureResult;
import android.hardware.camera2.TotalCaptureResult;
import android.hardware.camera2.params.Face;
import android.media.Image;
import android.media.ImageWriter;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Pair;
import android.util.Size;
import android.view.Surface;
import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.Executor;

final class OppoImageProcessor implements CaptureProcessorImpl,PreviewImageProcessorImpl {
    static final CaptureResult.Key<byte[]> BLINK_DEGREE=new CaptureResult.Key<>(
        "org.codeaurora.qcamera3.stats.blink_degree",byte[].class);
    private final boolean retouch;
    private final boolean bokeh;
    private final Rect activeArray;
    private final List<CaptureResult.Key> availableResultKeys;
    // Processing and close are serialized: buffers never outlive the session.
    private ImageWriter writer;
    private HandlerThread releaseThread;
    private boolean outputPending;
    OppoImageProcessor(boolean beauty,CameraCharacteristics c) {
        this(beauty?OppoExtender.Mode.RETOUCH:OppoExtender.Mode.AUTO,c);
    }
    OppoImageProcessor(OppoExtender.Mode mode,CameraCharacteristics c) {
        retouch=mode==OppoExtender.Mode.RETOUCH;
        bokeh=mode==OppoExtender.Mode.BOKEH;
        activeArray=c==null?null:c.get(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE);
        availableResultKeys=OppoExtender.resultKeys(c);
    }
    synchronized void close() {
        ImageWriter previous=writer;writer=null;outputPending=false;
        try { if(previous!=null)previous.close(); }
        finally {
            // A release callback may be waiting for this monitor. Quit without
            // joining it here; its writer-identity check rejects stale releases.
            if(releaseThread!=null)releaseThread.quitSafely();releaseThread=null;
        }
    }
    public synchronized void onOutputSurface(Surface surface,int format) {
        close();
        if(surface==null)return;
        try {
            writer=ImageWriter.newInstance(surface,2,ImageFormat.YUV_420_888);
            releaseThread=new HandlerThread("OppoBasicExtensionsRelease");releaseThread.start();
            writer.setOnImageReleasedListener(released->{ synchronized(OppoImageProcessor.this) {
                if(writer==released)outputPending=false;
            } },new Handler(releaseThread.getLooper()));
        } catch(RuntimeException failure) { close();throw failure; }
    }
    public void onResolutionUpdate(Size size) { validateSize(size); }
    public void onResolutionUpdate(Size size,Size postview) { validateSize(size); }
    private void validateSize(Size size) {
        if(size==null||size.getWidth()<32||size.getHeight()<32||
            (long)size.getWidth()*size.getHeight()>PixelPipeline.MAX_PIXELS)
            throw new IllegalArgumentException("Unsupported extension size");
    }
    public void onImageFormatUpdate(int format) {
        if(format!=ImageFormat.YUV_420_888)throw new IllegalArgumentException("YUV input required");
    }
    public void onPostviewOutputSurface(Surface surface) { throw new UnsupportedOperationException("No postview"); }
    public void processWithPostview(Map<Integer,Pair<Image,TotalCaptureResult>> images,
            ProcessResultImpl callback,Executor executor) { throw new UnsupportedOperationException("No postview"); }
    public void process(Map<Integer,Pair<Image,TotalCaptureResult>> images) { process(images,null,null); }
    public synchronized void process(Map<Integer,Pair<Image,TotalCaptureResult>> images,
            ProcessResultImpl callback,Executor executor) {
        try {
            if(images==null||images.size()!=4)throw new IllegalArgumentException("Four stages required");
            Image[] frames=new Image[4];TotalCaptureResult[] metadata=new TotalCaptureResult[4];
            for(int i=0;i<4;++i) {
                // Last exposure supplies the output timestamp, crop and 3A.
                Pair<Image,TotalCaptureResult> pair=images.get(i==0?3:i-1);
                if(pair==null||pair.first==null||pair.second==null)throw new IllegalArgumentException("Missing stage");
                frames[i]=pair.first;metadata[i]=pair.second;
            }
            output(frames,metadata,true,callback,executor);
        } finally {
            if(images!=null)for(Pair<Image,TotalCaptureResult> pair:images.values())
                if(pair!=null&&pair.first!=null)pair.first.close();
        }
    }
    public void process(Image image,TotalCaptureResult result) { process(image,result,null,null); }
    public synchronized void process(Image image,TotalCaptureResult result,
            ProcessResultImpl callback,Executor executor) {
        try { output(new Image[]{image},new TotalCaptureResult[]{result},false,callback,executor); }
        finally { if(image!=null)image.close(); }
    }
    static byte[] read(Image image) {
        int w=image.getWidth(),h=image.getHeight();
        if(image.getFormat()!=ImageFormat.YUV_420_888||(w&1)!=0||(h&1)!=0||
                w<32||h<32||(long)w*h>PixelPipeline.MAX_PIXELS)
            throw new IllegalArgumentException("Invalid image");
        if(!new Rect(0,0,w,h).equals(image.getCropRect()))throw new IllegalArgumentException("Unexpected image crop");
        byte[] output=new byte[w*h*3/2];copyPlanes(image,output,w,h,false);
        return output;
    }
    static void write(byte[] input,Image image,int width,int height) {
        if(image.getWidth()!=width||image.getHeight()!=height||image.getFormat()!=ImageFormat.YUV_420_888)
            throw new IllegalArgumentException("Output geometry changed");
        copyPlanes(image,input,width,height,true);
        image.setCropRect(new Rect(0,0,width,height));
    }
    private static void copyPlanes(Image image,byte[] packed,int width,int height,boolean toPlanes) {
        Image.Plane[] planes=image.getPlanes();
        if(planes.length!=3)throw new IllegalArgumentException("Missing YUV planes");
        ByteBuffer[] buffers=new ByteBuffer[3];int[] rows=new int[3],pixels=new int[3];
        for(int p=0;p<3;++p) {
            buffers[p]=planes[p].getBuffer();rows[p]=planes[p].getRowStride();pixels[p]=planes[p].getPixelStride();
        }
        YuvPlaneCopier.copy(buffers,rows,pixels,packed,width,height,toPlanes);
    }
    PixelPipeline.Face[] faces(TotalCaptureResult result,int w,int h) {
        if(result==null)return new PixelPipeline.Face[0];
        Face[] detected=result.get(CaptureResult.STATISTICS_FACES);
        Rect crop=result.get(CaptureResult.SCALER_CROP_REGION);
        if(crop==null)crop=activeArray;
        if(crop==null||crop.width()<=0||crop.height()<=0||detected==null)return new PixelPipeline.Face[0];
        // Each stream is aspect-cropped inside SCALER_CROP_REGION.
        double cw=crop.width(),ch=crop.height();
        if(cw/ch>(double)w/h)cw=ch*w/h;else ch=cw*h/w;
        double left=crop.centerX()-cw/2,top=crop.centerY()-ch/2;
        List<PixelPipeline.Face> mapped=new ArrayList<>();
        byte[] blink=result.get(BLINK_DEGREE);Long timestamp=result.get(CaptureResult.SENSOR_TIMESTAMP);
        for(int fi=0;fi<Math.min(10,detected.length);++fi) {
            Face face=detected[fi];if(face==null)continue;
            if(face.getScore()<50||mapped.size()==10)continue;
            Rect r=face.getBounds();
            int l=Math.max(0,(int)Math.round((r.left-left)*w/cw));
            int t=Math.max(0,(int)Math.round((r.top-top)*h/ch));
            int right=Math.min(w,(int)Math.round((r.right-left)*w/cw));
            int bottom=Math.min(h,(int)Math.round((r.bottom-top)*h/ch));
            int[] points=null;
            Point a=face.getLeftEyePosition(),b=face.getRightEyePosition(),m=face.getMouthPosition();
            if(a!=null&&b!=null&&m!=null)points=new int[]{
                (int)Math.round((a.x-left)*w/cw),(int)Math.round((a.y-top)*h/ch),
                (int)Math.round((b.x-left)*w/cw),(int)Math.round((b.y-top)*h/ch),
                (int)Math.round((m.x-left)*w/cw),(int)Math.round((m.y-top)*h/ch)};
            int lb=-1,rb=-1;
            if(blink!=null&&blink.length==detected.length*2) {
                lb=blink[fi*2]&255;rb=blink[fi*2+1]&255;if(lb>100||rb>100){lb=-1;rb=-1;}
            }
            if(right-l>=8&&bottom-t>=8)mapped.add(new PixelPipeline.Face(l,t,right,bottom,
                face.getId(),points,lb,rb,timestamp==null?0:timestamp));
        }
        return mapped.toArray(new PixelPipeline.Face[0]);
    }
    static boolean sameExposure(TotalCaptureResult a,TotalCaptureResult b) {
        if(a==null||b==null)return false;
        Long at=a.get(CaptureResult.SENSOR_EXPOSURE_TIME),bt=b.get(CaptureResult.SENSOR_EXPOSURE_TIME);
        Integer ai=a.get(CaptureResult.SENSOR_SENSITIVITY),bi=b.get(CaptureResult.SENSOR_SENSITIVITY);
        if(at==null||bt==null||ai==null||bi==null||at<=0||bt<=0||ai<=0||bi<=0)return false;
        double ratio=((double)at*ai)/((double)bt*bi);
        return ratio>.9&&ratio<1.1;
    }
    @SuppressWarnings("unchecked")
    private void output(Image[] frames,TotalCaptureResult[] metadata,boolean details,
            ProcessResultImpl callback,Executor executor) {
        if(writer==null)throw new IllegalStateException("Extension session is closed");
        // Only one output may await the consumer. Never enter a BufferQueue
        // dequeue while that output is retained: it could block this monitor
        // and prevent surface replacement or teardown from acquiring it.
        if(outputPending) {
            if(details)throw new IllegalStateException("Previous still output has not been released");
            return; // Drop a preview frame; its enclosing finally releases input.
        }
        int w=frames[0].getWidth(),h=frames[0].getHeight();
        byte[][] nv21=new byte[frames.length][];PixelPipeline.Face[][] faces=new PixelPipeline.Face[frames.length][];
        boolean[] eligible=new boolean[frames.length];
        for(int i=0;i<frames.length;++i) {
            if(frames[i].getWidth()!=w||frames[i].getHeight()!=h)throw new IllegalArgumentException("Mixed geometry");
            Long timestamp=metadata[i]==null?null:metadata[i].get(CaptureResult.SENSOR_TIMESTAMP);
            if(timestamp==null||timestamp.longValue()!=frames[i].getTimestamp())throw new IllegalArgumentException("Image/result mismatch");
            nv21[i]=read(frames[i]);faces[i]=faces(metadata[i],w,h);
            eligible[i]=i==0||sameExposure(metadata[0],metadata[i]);
        }
        byte[] pixels=PixelPipeline.process(nv21,w,h,faces,retouch,details,eligible);
        if(bokeh)pixels=PixelPipeline.bokeh(pixels,w,h,faces[0]);
        Image out=writer.dequeueInputImage();boolean queued=false;
        try {
            write(pixels,out,w,h);out.setTimestamp(frames[0].getTimestamp());
            outputPending=true;writer.queueInputImage(out);queued=true;
        } finally { if(!queued) { outputPending=false;out.close(); } }
        // Release input image mappings in the enclosing finally, including
        // failures. The platform proxy otherwise retains them until finalization.
        if(callback!=null&&metadata[0]!=null) {
            List<Pair<CaptureResult.Key,Object>> results=new ArrayList<>();
            for(CaptureResult.Key key:availableResultKeys) {
                Object value=metadata[0].get(key);if(value!=null)results.add(Pair.create(key,value));
            }
            long timestamp=frames[0].getTimestamp();
            Runnable done=()->callback.onCaptureCompleted(timestamp,results);
            if(executor==null)done.run();else executor.execute(done);
        }
    }
}
