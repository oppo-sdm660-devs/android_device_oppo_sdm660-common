/* SPDX-License-Identifier: BSD-3-Clause */
package androidx.camera.extensions.impl;

import java.util.function.BooleanSupplier;

/** Explicit open-eye preference from real, same-track burst observations.
 * Only bounded eye patches are fused. Frame zero remains the geometry/metadata
 * anchor; no whole face, mouth, skin or scene is replaced. */
final class FaceExpression {
    private static int value(byte[] frame,int w,int x,int y){return frame[y*w+x]&255;}
    private static boolean sameAppearance(byte[] a,byte[] b,int w,int h,PixelPipeline.Face f,
            PixelPipeline.Face g,BooleanSupplier stop) {
        int dx=(g.left+g.right-f.left-f.right)/2,dy=(g.top+g.bottom-f.top-f.bottom)/2;
        double error=0,dot=0,aa=0,bb=0;int n=0,bad=0;
        for(int y=f.top+2;y<f.bottom-2;y+=3) {
            PixelPipeline.checkCancelled(stop);
            for(int x=f.left+2;x<f.right-2;x+=3) {
                int sx=x+dx,sy=y+dy;if(sx<1||sx>=w-1||sy<1||sy>=h-1)continue;
                // Identity/appearance gate excludes the expression patches.
                boolean eye=false;double d=Math.hypot(f.landmarks[2]-f.landmarks[0],f.landmarks[3]-f.landmarks[1]);
                for(int e=0;e<2;++e)eye|=Math.hypot(x-f.landmarks[e*2],y-f.landmarks[e*2+1])<d*.30;
                if(eye)continue;
                int difference=Math.abs(value(a,w,x,y)-value(b,w,sx,sy));error+=difference;if(difference>24)++bad;
                int ax=value(a,w,x+1,y)-value(a,w,x-1,y),ay=value(a,w,x,y+1)-value(a,w,x,y-1);
                int bx=value(b,w,sx+1,sy)-value(b,w,sx-1,sy),by=value(b,w,sx,sy+1)-value(b,w,sx,sy-1);
                dot+=ax*bx+ay*by;aa+=ax*ax+ay*ay;bb+=bx*bx+by*by;++n;
                int p=w*h+(y/2)*w+(x&~1),q=w*h+(sy/2)*w+(sx&~1);
                if(Math.abs((a[p]&255)-(b[q]&255))+Math.abs((a[p+1]&255)-(b[q+1]&255))>16)return false;
            }
        }
        return n>=32&&error/n<=8&&bad<=n*.05&&Math.min(aa,bb)/n>=9&&dot/Math.sqrt(aa*bb)>=.88;
    }
    static byte[] apply(byte[] output,byte[][] frames,int w,int h,PixelPipeline.Face[][] faces,
            boolean[] eligible,int strength,BooleanSupplier stop) {
        PixelPipeline.checkCancelled(stop);
        long area=(long)w*h;
        if(w<32||h<32||(w&1)!=0||(h&1)!=0||area>PixelPipeline.MAX_PIXELS||
            output==null||output.length!=area*3/2||frames==null||frames.length<1||frames.length>4||
            faces==null||faces.length!=frames.length||(eligible!=null&&eligible.length!=frames.length)||
            strength<0||strength>100)throw new IllegalArgumentException("Invalid expression burst");
        for(byte[] frame:frames)if(frame==null||frame.length!=output.length)
            throw new IllegalArgumentException("Invalid expression frame");
        if(strength==0||frames.length<2||faces==null||faces[0]==null)return output;
        byte[] applied=new byte[w*h];
        for(int fi=0;fi<Math.min(10,faces[0].length);++fi) {
            PixelPipeline.Face f=faces[0][fi];
            if(!valid(f,w,h)||f.timestamp<=0||f.leftBlink<60||f.leftBlink>100||f.rightBlink<60||f.rightBlink>100||
                !unique(f,faces[0]))continue;
            int selected=-1,best=101;PixelPipeline.Face donor=null;
            for(int i=1;i<frames.length;++i) {
                if((eligible!=null&&!eligible[i])||faces[i]==null)continue;
                for(int gi=0;gi<Math.min(10,faces[i].length);++gi) {
                    PixelPipeline.Face g=faces[i][gi];
                    if(!valid(g,w,h)||g.id!=f.id||!unique(g,faces[i])||g.timestamp<=f.timestamp||
                        g.timestamp-f.timestamp>800_000_000L||g.leftBlink<0||g.rightBlink<0||
                        g.leftBlink>30||g.rightBlink>30||Math.max(g.leftBlink,g.rightBlink)>=best||
                        !FaceTracker.compatible(f,g))continue;
                    double eyeDistance=Math.hypot(f.landmarks[2]-f.landmarks[0],f.landmarks[3]-f.landmarks[1]);
                    boolean stable=true;
                    for(int p=0;p<6;p+=2)stable&=Math.hypot(f.landmarks[p]-g.landmarks[p],f.landmarks[p+1]-g.landmarks[p+1])<=eyeDistance*.10;
                    if(stable&&sameAppearance(frames[0],frames[i],w,h,f,g,stop)) {
                        selected=i;donor=g;best=Math.max(g.leftBlink,g.rightBlink);
                    }
                }
            }
            if(selected<0)continue;
            double radius=Math.hypot(f.landmarks[2]-f.landmarks[0],f.landmarks[3]-f.landmarks[1])*.23;
            for(int e=0;e<2;++e) {
                int cx=f.landmarks[e*2],cy=f.landmarks[e*2+1];
                int dx=donor.landmarks[e*2]-cx,dy=donor.landmarks[e*2+1]-cy;
                for(int y=Math.max(0,(int)(cy-radius*.70));y<Math.min(h,cy+radius*.70+1);++y) {
                    PixelPipeline.checkCancelled(stop);
                    for(int x=Math.max(0,(int)(cx-radius));x<Math.min(w,cx+radius+1);++x) {
                        int sx=x+dx,sy=y+dy;if(sx<0||sy<0||sx>=w||sy>=h)continue;
                        double r=(x-cx)*(x-cx)/(radius*radius)+(y-cy)*(y-cy)/(radius*radius*.49);
                        if(r>=1)continue;int p=y*w+x;
                        float weight=(float)((1-r)*(1-r)*strength*.008);
                        weight=Math.max(0,weight-(applied[p]&255)/255f);if(weight<=0)continue;
                        int target=value(frames[selected],w,sx,sy),base=value(frames[0],w,x,y);
                        // Prevent an uncontrolled dark/bright patch even after
                        // same-track and non-eye appearance qualification.
                        target=base+Math.max(-64,Math.min(64,target-base));
                        output[p]=(byte)Math.max(0,Math.min(255,Math.round((output[p]&255)*(1-weight)+target*weight)));
                        applied[p]=(byte)Math.min(255,(applied[p]&255)+Math.round(weight*255));
                    }
                }
            }
        }
        PixelPipeline.checkCancelled(stop);return output;
    }
    private static boolean valid(PixelPipeline.Face f,int w,int h) {
        return f!=null&&f.hasLandmarks()&&f.left>=0&&f.top>=0&&f.right<=w&&f.bottom<=h;
    }
    private static boolean unique(PixelPipeline.Face f,PixelPipeline.Face[] faces) {
        int n=0;for(int i=0;i<Math.min(10,faces.length);++i)if(faces[i]!=null&&faces[i].id==f.id)++n;
        return n==1;
    }
}
