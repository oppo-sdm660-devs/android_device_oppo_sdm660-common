/* SPDX-License-Identifier: BSD-3-Clause */
package androidx.camera.extensions.impl;

/** Bounded HAL3 AUTO scene evidence. This is not semantic scene recognition.
 * Callers supply timestamp-paired preview YUV and standard, converged results,
 * and serialize updates/selection with the session lock. No HAL1 ASD ABI is used.
 */
final class OppoAutoCapturePolicy {
    static final long MAX_AGE_NS=1_000_000_000L;
    static final int GRID_WIDTH=32,GRID_HEIGHT=24;
    private static final long MAX_PAIR_GAP_NS=250_000_000L;
    private long sensorTimestamp,arrival,exposure;
    private int sensitivity,stableFrames,darkFrames,hdrFrames;
    private boolean observed,moving;
    private int[] previous;

    void reset() {
        sensorTimestamp=arrival=exposure=0;sensitivity=stableFrames=darkFrames=hdrFrames=0;
        observed=moving=false;previous=null;
    }

    /** Input is packed NV21; at most 768 luma samples and 256 histogram bins.
     * Both arrival clocks are monotonic receipt times, not sensor timestamps:
     * Camera2 does not guarantee SENSOR_TIMESTAMP has the nanoTime timebase.
     */
    void update(long timestamp,long exposureNs,int iso,boolean automaticConverged,
            byte[] nv21,int width,int height,long imageArrival,long resultArrival,long now) {
        // Old/duplicate pairs cannot contribute extra consecutive evidence.
        if(observed&&timestamp<=sensorTimestamp)return;
        if(!automaticConverged||timestamp<=0||exposureNs<=0||iso<=0||
                width<32||height<24||nv21==null||(long)width*height>nv21.length||
                !fresh(imageArrival,now)||!fresh(resultArrival,now)||
                Math.abs(imageArrival-resultArrival)>MAX_PAIR_GAP_NS) { reset();return; }
        if(observed&&(timestamp-sensorTimestamp>MAX_PAIR_GAP_NS||!fresh(arrival,now)))reset();
        int[] sample=new int[GRID_WIDTH*GRID_HEIGHT],histogram=new int[256];
        int sum=0,shadows=0,highlights=0;
        for(int y=0;y<GRID_HEIGHT;++y)for(int x=0;x<GRID_WIDTH;++x) {
            int row=(int)(((2L*y+1)*height)/(2*GRID_HEIGHT));
            int column=(int)(((2L*x+1)*width)/(2*GRID_WIDTH));
            int value=nv21[row*width+column]&255;
            sample[y*GRID_WIDTH+x]=value;++histogram[value];sum+=value;
            if(value<=56)++shadows;if(value>=220)++highlights;
        }
        boolean comparable=previous!=null&&withinTenPercent(exposureNs,exposure)&&
            withinTenPercent(iso,sensitivity);
        moving=false;
        if(comparable) {
            int previousSum=0;for(int value:previous)previousSum+=value;
            double mean=(double)sum/sample.length,previousMean=(double)previousSum/sample.length;
            int changed=0;double difference=0;
            // Remove a global brightness offset; local changes still reject
            // subject/camera motion. This is a conservative motion gate, not
            // optical flow or a promise to detect sub-grid moving subjects.
            for(int i=0;i<sample.length;++i) {
                double delta=Math.abs((sample[i]-mean)-(previous[i]-previousMean));
                difference+=delta;if(delta>24)++changed;
            }
            moving=difference/sample.length>10||changed*100>sample.length*12;
        }
        stableFrames=comparable&&!moving?Math.min(3,stableFrames+1):0;
        boolean dark=exposureNs>=25_000_000L&&iso>=800;
        // A bright-tail and a shadow-tail must coexist in real preview data.
        // High ISO or a long exposure alone must never turn on AUTO HDR.
        boolean highDynamicRange=percentile(histogram,sample.length,90)-
            percentile(histogram,sample.length,10)>=128&&
            shadows*100>=sample.length*12&&highlights*100>=sample.length*4;
        darkFrames=comparable&&!moving&&dark?Math.min(3,darkFrames+1):0;
        hdrFrames=comparable&&!moving&&highDynamicRange?Math.min(3,hdrFrames+1):0;
        previous=sample;observed=true;sensorTimestamp=timestamp;
        arrival=Math.min(imageArrival,resultArrival);exposure=exposureNs;sensitivity=iso;
    }

    private static boolean withinTenPercent(long a,long b) {
        return b>0&&(double)a/b>=.9&&(double)a/b<=1.1;
    }
    private static boolean fresh(long received,long now) {
        return now>=received&&now-received<=MAX_AGE_NS;
    }
    private static int percentile(int[] histogram,int count,int percent) {
        int target=(count*percent+99)/100,total=0;
        for(int value=0;value<histogram.length;++value)if((total+=histogram[value])>=target)return value;
        return 255;
    }

    /** Select only backends advertised by this camera. Unknown scenes use the
     * MFNR fallback; known motion bypasses multiframe enhancement altogether.
     * Detail enhancement needs short, low-ISO, stable preview evidence.
     */
    int select(long now,boolean hdr,boolean night,boolean mfnr,boolean superphoto) {
        int fallback=mfnr?3:0;
        if(!observed||!fresh(arrival,now))return fallback;
        if(moving)return 0;
        if(stableFrames<3)return fallback;
        if(night&&darkFrames==3)return 2;
        if(hdrFrames==3)return hdr?1:fallback;
        if(superphoto&&exposure<=16_666_667L&&sensitivity<=400)return 4;
        return fallback;
    }
}
