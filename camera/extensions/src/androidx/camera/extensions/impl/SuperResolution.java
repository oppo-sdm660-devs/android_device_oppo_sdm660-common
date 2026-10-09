/* SPDX-License-Identifier: BSD-3-Clause */
package androidx.camera.extensions.impl;

import java.util.function.BooleanSupplier;

/** Reconstructs a 2x grid from independent, physically captured YUV images.
 * Uses the caller-supplied first exposure as the anchor (array index 0), as
 * required by advanced Extensions' onCaptureStarted/onCaptureCompleted pair.
 * No smaller input is synthesized by downsampling an output-sized image.
 */
final class SuperResolution {
    static final int MAX_OUTPUT_PIXELS=8*1024*1024;
    static final class Result {
        final byte[] nv21;final boolean reconstructed;
        Result(byte[] p,boolean r){nv21=p;reconstructed=r;}
    }
    private static int value(byte[] data,int w,int h,int x,int y){
        return data[Math.max(0,Math.min(h-1,y))*w+Math.max(0,Math.min(w-1,x))]&255;
    }
    private static float sample(byte[] data,int w,int h,float x,float y){
        int ix=(int)Math.floor(x),iy=(int)Math.floor(y);float fx=x-ix,fy=y-iy;
        return value(data,w,h,ix,iy)*(1-fx)*(1-fy)+value(data,w,h,ix+1,iy)*fx*(1-fy)+
            value(data,w,h,ix,iy+1)*(1-fx)*fy+value(data,w,h,ix+1,iy+1)*fx*fy;
    }
    private static float lowpass(byte[] data,int w,int h,float x,float y){
        return (sample(data,w,h,x,y)*4+sample(data,w,h,x-1,y)+sample(data,w,h,x+1,y)+
            sample(data,w,h,x,y-1)+sample(data,w,h,x,y+1))/8;
    }
    private static double error(byte[] a,byte[] b,int w,int h,float dx,float dy){
        double sum=0;int n=0,step=Math.max(2,Math.max(w,h)/64);
        for(int y=12;y<h-12;y+=step)for(int x=12;x<w-12;x+=step){
            sum+=Math.min(24,Math.abs(lowpass(a,w,h,x,y)-lowpass(b,w,h,x+dx,y+dy)));++n;
        }
        return n==0?100:sum/n;
    }
    private static byte clamp(float value){return (byte)Math.max(0,Math.min(255,Math.round(value)));}
    static Result reconstruct(byte[][] frames,int width,int height,boolean[] eligible,BooleanSupplier cancelled){
        if(frames==null||frames.length==0||frames.length>4||width<32||height<32||(width&1)!=0||(height&1)!=0||
            (long)width*height>MAX_OUTPUT_PIXELS/4||eligible==null||eligible.length!=frames.length)
            throw new IllegalArgumentException("Invalid SR input");
        if(cancelled==null)cancelled=()->false;
        int w=width*2,h=height*2;
        for(byte[] frame:frames)if(frame==null||frame.length!=(long)width*height*3/2)
            throw new IllegalArgumentException("Invalid SR image");
        float[] dx=new float[frames.length],dy=new float[frames.length];boolean[] accepted=new boolean[frames.length];
        accepted[0]=true;boolean diversity=false;
        for(int i=1;i<frames.length;++i){
            PixelPipeline.checkCancelled(cancelled);if(!eligible[i])continue;
            double best=error(frames[0],frames[i],width,height,0,0);
            if(best>1){
                // Check the local subpixel basin before a coarse global
                // search can lock onto a distant repeating texture.
                for(int y=-2;y<=2;++y)for(int x=-2;x<=2;++x){
                    double cost=error(frames[0],frames[i],width,height,x*.5f,y*.5f);
                    if(cost<best-.025){best=cost;dx[i]=x*.5f;dy[i]=y*.5f;}
                }
                for(int y=-8;y<=8;y+=2)for(int x=-8;x<=8;x+=2){
                    double cost=error(frames[0],frames[i],width,height,x,y);
                    if(cost<best-.05){best=cost;dx[i]=x;dy[i]=y;}
                }
                for(float step:new float[]{1,.5f}){
                    float cx=dx[i],cy=dy[i];
                    for(int y=-2;y<=2;++y)for(int x=-2;x<=2;++x){
                        double cost=error(frames[0],frames[i],width,height,cx+x*step,cy+y*step);
                        if(cost<best-.025){best=cost;dx[i]=cx+x*step;dy[i]=cy+y*step;}
                    }
                }
            }
            accepted[i]=best<14;
            if(accepted[i]&&(Math.abs(dx[i]-Math.round(dx[i]))>.1||Math.abs(dy[i]-Math.round(dy[i]))>.1))diversity=true;
        }
        byte[] output=new byte[w*h*3/2];
        for(int y=0;y<h;++y){
            PixelPipeline.checkCancelled(cancelled);
            for(int x=0;x<w;++x){
                float rx=x*.5f,ry=y*.5f,anchor=sample(frames[0],width,height,rx,ry),local=lowpass(frames[0],width,height,rx,ry);
                float sum=diversity?0:anchor*2,weights=diversity?0:2;
                for(int i=diversity?0:1;i<frames.length;++i){
                    if(!accepted[i])continue;float sx=rx+dx[i],sy=ry+dy[i];
                    if(sx<1||sy<1||sx>=width-2||sy>=height-2)continue;
                    // On a 2x grid, a registered half-pixel observation is a
                    // measured sample at the missing phase. Averaging bilinear
                    // estimates at every phase first blurs that extra detail.
                    float phase=diversity?Math.max(0,1-2*Math.abs(sx-Math.round(sx)))*
                        Math.max(0,1-2*Math.abs(sy-Math.round(sy))):1;
                    if(phase==0)continue;
                    float v=diversity?value(frames[i],width,height,Math.round(sx),Math.round(sy)):
                        sample(frames[i],width,height,sx,sy);
                    float difference=Math.abs(local-lowpass(frames[i],width,height,sx,sy));
                    if(difference>14||Math.abs(v-anchor)>40)continue;
                    float weight=phase*(14-difference)/14;sum+=v*weight;weights+=weight;
                }
                output[y*w+x]=clamp(weights>0?sum/weights:anchor);
            }
        }
        for(int y=0;y<h/2;++y){
            PixelPipeline.checkCancelled(cancelled);
            for(int x=0;x<w;x+=2)for(int c=0;c<2;++c)
                output[w*h+y*w+x+c]=frames[0][width*height+(y/2)*width+(x/4)*2+c];
        }
        PixelPipeline.checkCancelled(cancelled);return new Result(output,diversity);
    }
}
