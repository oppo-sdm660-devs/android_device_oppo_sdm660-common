/* SPDX-License-Identifier: BSD-3-Clause */
package androidx.camera.extensions.impl;

import java.nio.ByteBuffer;

/** Android YUV_420_888 planes may be planar or interleaved, padded and offset. */
final class YuvPlaneCopier {
    static void copy(ByteBuffer[] planes,int[] rows,int[] pixels,byte[] nv21,
            int width,int height,boolean toPlanes) {
        if(planes==null||rows==null||pixels==null||planes.length!=3||rows.length!=3||pixels.length!=3||
                width<=0||height<=0||(width&1)!=0||(height&1)!=0||
                (long)width*height>SuperResolution.MAX_OUTPUT_PIXELS||nv21==null||nv21.length!=(long)width*height*3/2)
            throw new IllegalArgumentException("Invalid YUV layout");
        // Validate every plane before writing any output bytes.
        for(int p=0;p<3;++p) {
            int w=p==0?width:width/2,h=p==0?height:height/2;
            if(planes[p]==null||rows[p]<=0||pixels[p]<=0||
                rows[p]<(long)(w-1)*pixels[p]+1||
                (long)(h-1)*rows[p]+(long)(w-1)*pixels[p]+planes[p].position()>=planes[p].limit()||
                (toPlanes&&planes[p].isReadOnly()))throw new IllegalArgumentException("Invalid YUV plane");
        }
        for(int p=0;p<3;++p) {
            int w=p==0?width:width/2,h=p==0?height:height/2;
            ByteBuffer buffer=planes[p].duplicate();int start=buffer.position();
            for(int y=0;y<h;++y)for(int x=0;x<w;++x) {
                int packed=p==0?y*width+x:width*height+y*width+x*2+(p==1?1:0);
                int planar=start+y*rows[p]+x*pixels[p];
                if(toPlanes)buffer.put(planar,nv21[packed]);else nv21[packed]=buffer.get(planar);
            }
        }
    }
}
