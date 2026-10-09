/* SPDX-License-Identifier: BSD-3-Clause */
package androidx.camera.extensions.impl;

import java.util.HashMap;
import java.util.Map;

/** Bounded smoothing of real FULL landmarks. No prediction or face-box-derived
 * point is emitted, and a lost face has no retained output. Worker-thread only. */
final class FaceTracker {
    private final Map<Integer,PixelPipeline.Face> previous=new HashMap<>();
    private int width,height;private long lastTimestamp;
    void clear(){previous.clear();width=height=0;lastTimestamp=0;}
    PixelPipeline.Face[] update(PixelPipeline.Face[] current,int w,int h,long timestamp) {
        if(w!=width||h!=height||timestamp<=lastTimestamp||timestamp-lastTimestamp>250_000_000L)previous.clear();
        width=w;height=h;lastTimestamp=timestamp;
        if(current==null){previous.clear();return new PixelPipeline.Face[0];}
        int count=Math.min(10,current.length);
        Map<Integer,PixelPipeline.Face> next=new HashMap<>();PixelPipeline.Face[] output=new PixelPipeline.Face[count];
        for(int i=0;i<count;++i) {
            PixelPipeline.Face f=current[i];output[i]=f;
            if(f==null||!f.hasLandmarks())continue;
            boolean duplicate=false;
            for(int j=0;j<count;++j)duplicate|=j!=i&&current[j]!=null&&current[j].id==f.id;
            if(duplicate||timestamp<=0||f.timestamp!=timestamp) {
                // Ambiguous identity or mismatched metadata may still serve the
                // coarse rectangle effects, never automatic point effects.
                output[i]=new PixelPipeline.Face(f.left,f.top,f.right,f.bottom);continue;
            }
            PixelPipeline.Face old=previous.get(f.id);int[] points=f.landmarks.clone();
            if(old!=null&&old.hasLandmarks()&&compatible(old,f)) {
                for(int p=0;p<6;++p)points[p]=Math.round(f.landmarks[p]*.75f+old.landmarks[p]*.25f);
            }
            PixelPipeline.Face tracked=new PixelPipeline.Face(f.left,f.top,f.right,f.bottom,
                f.id,points,f.leftBlink,f.rightBlink,f.timestamp);
            // Rounding or rapid boundary changes must never turn an observation
            // into a point outside the currently observed face.
            if(!tracked.hasLandmarks())tracked=f;
            output[i]=tracked;next.put(f.id,tracked);
        }
        previous.clear();previous.putAll(next);return output;
    }
    static boolean compatible(PixelPipeline.Face a,PixelPipeline.Face b) {
        double aw=a.right-a.left,ah=a.bottom-a.top,bw=b.right-b.left,bh=b.bottom-b.top;
        if(bw<aw*.80||bw>aw*1.25||bh<ah*.80||bh>ah*1.25||
            Math.abs(a.left+a.right-b.left-b.right)>aw*.5||
            Math.abs(a.top+a.bottom-b.top-b.bottom)>ah*.5)return false;
        for(int i=0;i<6;i+=2)if(Math.hypot(a.landmarks[i]-b.landmarks[i],a.landmarks[i+1]-b.landmarks[i+1])>Math.min(aw,ah)*.25)return false;
        return true;
    }
}
