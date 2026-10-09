/* SPDX-License-Identifier: BSD-3-Clause */
package androidx.camera.extensions.impl;

import java.util.concurrent.CancellationException;
import java.util.function.BooleanSupplier;

/** Bounded CPU YUV processing shared by the preview and still extension.
 * No Android dependencies: numerical behavior is covered by host tests.
 * Input 0 is the metadata and geometry anchor; input pixels are never modified.
 */
final class PixelPipeline {
    static final int MAX_PIXELS = 4 * 1024 * 1024;
    private static final int MAX_FACES = 10;
    static final class Face {
        final int left, top, right, bottom;
        final int id,leftBlink,rightBlink;final int[] landmarks;final long timestamp;
        Face(int l, int t, int r, int b) { this(l,t,r,b,-1,null,-1,-1,0); }
        Face(int l,int t,int r,int b,int identity,int[] points,int blinkLeft,int blinkRight,long time) {
            left=l;top=t;right=r;bottom=b;id=identity;
            landmarks=points==null?null:points.clone();leftBlink=blinkLeft;rightBlink=blinkRight;timestamp=time;
        }
        boolean hasLandmarks() {
            if(id<0||landmarks==null||landmarks.length!=6)return false;
            for(int i=0;i<6;i+=2)if(landmarks[i]<left||landmarks[i]>=right||landmarks[i+1]<top||landmarks[i+1]>=bottom)return false;
            double ax=landmarks[2]-landmarks[0],ay=landmarks[3]-landmarks[1];
            double bx=landmarks[4]-(landmarks[0]+landmarks[2])*.5,by=landmarks[5]-(landmarks[1]+landmarks[3])*.5;
            double distance=ax*ax+ay*ay,area=Math.abs(ax*by-ay*bx);
            return distance>=64&&area>distance*.12&&area<distance*2;
        }
    }
    static final class Offset {
        int x, y;
        boolean usable;
    }
    private static int value(byte[] image, int w, int h, int x, int y) {
        return image[Math.max(0,Math.min(h-1,y))*w+Math.max(0,Math.min(w-1,x))]&255;
    }
    private static int lowpass(byte[] image, int w, int h, int x, int y) {
        return (value(image,w,h,x,y)*4+value(image,w,h,x-1,y)+value(image,w,h,x+1,y)+
                value(image,w,h,x,y-1)+value(image,w,h,x,y+1))/8;
    }
    private static double error(byte[] a, byte[] b, int w, int h, int dx, int dy,
            BooleanSupplier cancelled) {
        long sum=0; int n=0,step=Math.max(2,Math.max(w,h)/64);
        for(int y=12;y<h-12;y+=step) {
            checkCancelled(cancelled);
            for(int x=12;x<w-12;x+=step) {
            sum+=Math.min(24,Math.abs(lowpass(a,w,h,x,y)-lowpass(b,w,h,x+dx,y+dy))); ++n;
            }
        }
        return n==0?100:(double)sum/n;
    }
    private static Offset align(byte[] a, byte[] b, int w, int h,BooleanSupplier cancelled) {
        Offset o=new Offset(); double best=error(a,b,w,h,0,0,cancelled);
        if(best>1) {
            for(int dy=-8;dy<=8;dy+=2) for(int dx=-8;dx<=8;dx+=2) {
                double score=error(a,b,w,h,dx,dy,cancelled);
                if(score<best-.05) { best=score; o.x=dx; o.y=dy; }
            }
            int cx=o.x,cy=o.y;
            for(int dy=-1;dy<=1;++dy) for(int dx=-1;dx<=1;++dx) {
                double score=error(a,b,w,h,cx+dx,cy+dy,cancelled);
                if(score<best-.05) { best=score; o.x=cx+dx; o.y=cy+dy; }
            }
        }
        o.usable=best<14; return o;
    }
    private static byte clamp(float v) { return (byte)Math.max(0,Math.min(255,Math.round(v))); }
    private static boolean valid(Face f,int w,int h) {
        return f!=null && f.left>=0 && f.top>=0 && f.right<=w && f.bottom<=h &&
                f.right-f.left>=8 && f.bottom-f.top>=8;
    }
    private static float mask(Face f,int x,int y) {
        float nx=(2f*x-f.left-f.right)/(f.right-f.left);
        float ny=(2f*y-f.top-f.bottom)/(f.bottom-f.top);
        return Math.max(0,Math.min(1,(1-nx*nx-ny*ny)*3));
    }
    private static boolean matching(Face a,Face[] faces,Offset o,int w,int h) {
        if(faces==null)return false;
        for(int i=0;i<Math.min(MAX_FACES,faces.length);++i) {
            Face b=faces[i];if(!valid(b,w,h))continue;
            int width=Math.max(0,Math.min(a.right,b.right-o.x)-Math.max(a.left,b.left-o.x));
            int height=Math.max(0,Math.min(a.bottom,b.bottom-o.y)-Math.max(a.top,b.top-o.y));
            if(width*height>.88f*Math.max((a.right-a.left)*(a.bottom-a.top),(b.right-b.left)*(b.bottom-b.top)))return true;
        }
        return false;
    }
    private static float unit(float x) { return Math.max(0,Math.min(1,x)); }
    private static float window(float x,float low,float high,float fade) {
        return unit((x-low+fade)/fade)*unit((high+fade-x)/fade);
    }
    private static int chroma(byte[] frame,int w,int h,int x,int y,int component) {
        x=Math.max(0,Math.min(w-1,x));y=Math.max(0,Math.min(h-1,y));
        return frame[w*h+(y/2)*w+(x&~1)+component]&255;
    }
    /** Broad, soft YCbCr plausibility, not a skin classifier or a face detector.
     * All processing still requires the anchor frame's detected face rectangle. */
    private static float skin(byte[] frame,int w,int h,int x,int y) {
        return window(chroma(frame,w,h,x,y,0),136,174,12)*
                window(chroma(frame,w,h,x,y,1),88,123,14)*
                window(value(frame,w,h,x,y),35,205,20);
    }
    private static float featureProtection(Face face,int x,int y) {
        float nx=(x-face.left)/(float)(face.right-face.left);
        float ny=(y-face.top)/(float)(face.bottom-face.top);
        // Protect both eyes/brows, nose and lips even when they share skin chroma.
        float eyes=window(ny,.24f,.47f,.07f)*
                Math.max(window(nx,.12f,.40f,.08f),window(nx,.60f,.88f,.08f));
        float nose=window(nx,.40f,.60f,.07f)*window(ny,.40f,.72f,.08f);
        float mouth=window(nx,.25f,.75f,.07f)*window(ny,.70f,.85f,.06f);
        return 1-Math.max(eyes,Math.max(nose,mouth));
    }
    private static final class FaceLight {
        float mean,left,right;
        boolean usable;
    }
    private static FaceLight measureLight(byte[] frame,int w,int h,Face face,
            BooleanSupplier cancelled) {
        FaceLight light=new FaceLight();float sum=0,weight=0,ls=0,lw=0,rs=0,rw=0;
        int step=Math.max(1,Math.min(face.right-face.left,face.bottom-face.top)/32);
        int center=(face.left+face.right)/2;
        for(int y=face.top;y<face.bottom;y+=step) {
            checkCancelled(cancelled);
            for(int x=face.left;x<face.right;x+=step) {
                float a=mask(face,x,y)*skin(frame,w,h,x,y)*featureProtection(face,x,y);
                int v=lowpass(frame,w,h,x,y);
                sum+=v*a;weight+=a;
                if(x<center){ls+=v*a;lw+=a;}else{rs+=v*a;rw+=a;}
            }
        }
        light.usable=weight>=12&&lw>=4&&rw>=4;
        if(light.usable){light.mean=sum/weight;light.left=ls/lw;light.right=rs/rw;}
        return light;
    }
    private static float fillLight(FaceLight light,Face face,int x,float luma) {
        if(!light.usable)return 0;
        float lowExposure=Math.min(7,Math.max(0,(116-light.mean)*.12f));
        float position=(2f*x-face.left-face.right)/(face.right-face.left);
        float shadowSide=unit((light.right-light.left)*-position/24f);
        float sideFill=Math.min(5,Math.max(0,Math.abs(light.right-light.left)-10)*.10f)*shadowSide;
        // Limit highlights and very dark unreliable samples; no fixed light direction.
        return Math.min(9,lowExposure+sideFill)*unit((205-luma)/90)*unit((luma-18)/24);
    }
    private static final class FaceQuality {
        double detail,noise,clipped;
        double score() { return detail/(1+noise*.3); }
    }
    private static FaceQuality quality(byte[] frame,int w,int h,Face face,int dx,int dy,
            BooleanSupplier cancelled) {
        FaceQuality q=new FaceQuality();int n=0,flat=0;
        int step=Math.max(2,Math.min(face.right-face.left,face.bottom-face.top)/32);
        for(int y=face.top+2;y<face.bottom-2;y+=step) {
            checkCancelled(cancelled);
            for(int x=face.left+2;x<face.right-2;x+=step) {
                if(mask(face,x,y)<.5f)continue;
                int sx=x+dx,sy=y+dy,v=value(frame,w,h,sx,sy);
                int gx=lowpass(frame,w,h,sx+1,sy)-lowpass(frame,w,h,sx-1,sy);
                int gy=lowpass(frame,w,h,sx,sy+1)-lowpass(frame,w,h,sx,sy-1);
                int gradient=Math.abs(gx)+Math.abs(gy);
                q.detail+=Math.min(48,gradient);
                if(gradient<12){q.noise+=Math.abs(v-lowpass(frame,w,h,sx,sy));++flat;}
                if(v<12||v>243)++q.clipped;
                ++n;
            }
        }
        if(n==0)return q;
        q.detail/=n;q.noise=flat==0?0:q.noise/flat;q.clipped/=n;return q;
    }
    /** Boxes are not identity evidence. Permit only same-geometry, photometrically
     * consistent detail residuals, never replace the face or select an expression. */
    private static boolean consistentFace(byte[] anchor,byte[] donor,int w,int h,Face face,
            Offset offset,BooleanSupplier cancelled) {
        double residual=0,bias=0,chromaResidual=0,dot=0,aa=0,bb=0;int n=0,moving=0;
        int step=Math.max(2,Math.min(face.right-face.left,face.bottom-face.top)/32);
        for(int y=face.top+2;y<face.bottom-2;y+=step) {
            checkCancelled(cancelled);
            for(int x=face.left+2;x<face.right-2;x+=step) {
                if(mask(face,x,y)<.5f)continue;
                int sx=x+offset.x,sy=y+offset.y;
                if(sx<2||sy<2||sx>=w-2||sy>=h-2)return false;
                int delta=lowpass(donor,w,h,sx,sy)-lowpass(anchor,w,h,x,y);
                residual+=Math.abs(delta);bias+=delta;if(Math.abs(delta)>10)++moving;
                chromaResidual+=Math.abs(chroma(anchor,w,h,x,y,0)-chroma(donor,w,h,sx,sy,0))+
                        Math.abs(chroma(anchor,w,h,x,y,1)-chroma(donor,w,h,sx,sy,1));
                int ax=lowpass(anchor,w,h,x+1,y)-lowpass(anchor,w,h,x-1,y);
                int ay=lowpass(anchor,w,h,x,y+1)-lowpass(anchor,w,h,x,y-1);
                int bx=lowpass(donor,w,h,sx+1,sy)-lowpass(donor,w,h,sx-1,sy);
                int by=lowpass(donor,w,h,sx,sy+1)-lowpass(donor,w,h,sx,sy-1);
                dot+=ax*bx+ay*by;aa+=ax*ax+ay*ay;bb+=bx*bx+by*by;++n;
            }
        }
        return n>=24&&residual/n<=4&&Math.abs(bias)/n<=2&&chromaResidual/n<=6&&
                moving<=n*.025&&aa>n*2&&bb>n*2&&dot/Math.sqrt(aa*bb)>=.94;
    }
    static byte[] process(byte[][] frames,int w,int h,Face[][] faces,boolean retouch,
            boolean details,boolean[] eligible) {
        return process(frames,w,h,faces,retouch,details,eligible,()->false);
    }
    static void checkCancelled(BooleanSupplier cancelled) {
        if(cancelled.getAsBoolean())throw new CancellationException();
    }
    /** Face-guided portrait blur. The soft head/shoulder prior is deliberately
     * conservative; it does not infer scene depth or claim subject segmentation.
     * Without a usable face the original scene is preserved. */
    static byte[] bokeh(byte[] image,int w,int h,Face[] faces) {
        return bokeh(image,w,h,faces,()->false);
    }
    static byte[] bokeh(byte[] image,int w,int h,Face[] faces,BooleanSupplier cancelled) {
        return bokeh(image,w,h,faces,100,cancelled);
    }
    static byte[] bokeh(byte[] image,int w,int h,Face[] faces,int strength,
            BooleanSupplier cancelled) {
        checkCancelled(cancelled);
        long pixels=(long)w*h;
        if(w<32||h<32||(w&1)!=0||(h&1)!=0||pixels>MAX_PIXELS||image==null||
                image.length!=pixels*3/2)throw new IllegalArgumentException("Invalid portrait image");
        checkStrength(strength);
        byte[] output=image.clone();
        if(strength==0)return output;
        boolean usable=false;
        if(faces!=null)for(int i=0;i<Math.min(MAX_FACES,faces.length);++i)usable|=valid(faces[i],w,h);
        if(!usable)return output;
        byte[] foreground=new byte[(int)pixels];
        for(int i=0;i<Math.min(MAX_FACES,faces.length);++i) {
            Face face=faces[i];if(!valid(face,w,h))continue;
            float fw=face.right-face.left,fh=face.bottom-face.top;
            float cx=(face.left+face.right)*.5f,cy=(face.top+face.bottom)*.5f;
            // Expand the head protection and keep the probable shoulders/torso.
            // A union also protects multiple detected people without face IDs.
            portraitMask(foreground,w,h,cx,cy,fw*.85f,fh*.95f,cancelled);
            portraitMask(foreground,w,h,cx,face.bottom+fh*1.15f,fw*1.45f,fh*2.0f,cancelled);
        }
        foreground=guideMask(image,foreground,w,h,cancelled);
        byte[] blurred=image.clone(),scratch=new byte[image.length];
        int radius=Math.max(2,Math.min(20,Math.min(w,h)/80));
        for(int pass=0;pass<3;++pass) {
            blurPlane(blurred,scratch,foreground,w,h,0,1,radius,cancelled);
            blurPlane(blurred,scratch,foreground,w/2,h/2,w*h,2,Math.max(1,radius/2),cancelled);
            byte[] swap=blurred;blurred=scratch;scratch=swap;
        }
        for(int y=0;y<h;++y) {
            checkCancelled(cancelled);
            for(int x=0;x<w;++x) {
                int index=y*w+x,a=foreground[index]&255;
                output[index]=(byte)(((image[index]&255)*a+(blurred[index]&255)*(255-a)+127)/255);
            }
        }
        for(int y=0;y<h/2;++y) {
            checkCancelled(cancelled);
            for(int x=0;x<w;x+=2) {
                int p=y*2*w+x;
                int a=((foreground[p]&255)+(foreground[p+1]&255)+
                        (foreground[p+w]&255)+(foreground[p+w+1]&255)+2)/4;
                for(int c=0;c<2;++c) {
                    int index=w*h+y*w+x+c;
                    output[index]=(byte)(((image[index]&255)*a+(blurred[index]&255)*(255-a)+127)/255);
                }
            }
        }
        checkCancelled(cancelled);
        return blendStrength(image,output,strength,cancelled);
    }
    private static void portraitMask(byte[] mask,int w,int h,float cx,float cy,
            float rx,float ry,BooleanSupplier cancelled) {
        int left=Math.max(0,(int)Math.floor(cx-rx*1.2f)),right=Math.min(w,(int)Math.ceil(cx+rx*1.2f));
        int top=Math.max(0,(int)Math.floor(cy-ry*1.2f)),bottom=Math.min(h,(int)Math.ceil(cy+ry*1.2f));
        for(int y=top;y<bottom;++y) {
            checkCancelled(cancelled);
            float ny=(y+.5f-cy)/ry;
            for(int x=left;x<right;++x) {
                float nx=(x+.5f-cx)/rx;
                float a=Math.max(0,Math.min(1,(1.4f-nx*nx-ny*ny)/.6f));
                int value=Math.round(255*a*a*(3-2*a));
                if(value>(mask[y*w+x]&255))mask[y*w+x]=(byte)value;
            }
        }
    }
    /** Joint color/luma guidance only adjusts the uncertain geometric boundary by
     * at most 32/255. It cannot grow a subject, recover hair, or infer scene depth. */
    private static byte[] guideMask(byte[] image,byte[] prior,int w,int h,
            BooleanSupplier cancelled) {
        byte[] refined=prior.clone();int spacing=Math.max(1,Math.min(w,h)/320);
        for(int y=0;y<h;++y) {
            checkCancelled(cancelled);
            for(int x=0;x<w;++x) {
                int index=y*w+x,a=prior[index]&255;
                if(a<=5||a>=250)continue;
                int center=lowpass(image,w,h,x,y);
                int cv=chroma(image,w,h,x,y,0),cu=chroma(image,w,h,x,y,1);
                float sum=0,weights=0;
                for(int dy=-2;dy<=2;++dy)for(int dx=-2;dx<=2;++dx) {
                    int sx=Math.max(0,Math.min(w-1,x+dx*spacing));
                    int sy=Math.max(0,Math.min(h-1,y+dy*spacing));
                    float dl=(lowpass(image,w,h,sx,sy)-center)/12f;
                    float dv=(chroma(image,w,h,sx,sy,0)-cv)/16f;
                    float du=(chroma(image,w,h,sx,sy,1)-cu)/16f;
                    float weight=1f/((1+dx*dx+dy*dy)*(1+dl*dl+dv*dv+du*du));
                    sum+=(prior[sy*w+sx]&255)*weight;weights+=weight;
                }
                refined[index]=(byte)Math.max(a-32,Math.min(a+32,Math.round(sum/weights)));
            }
        }
        return refined;
    }
    private static int backgroundWeight(byte[] mask,int w,int x,int y,int step) {
        int alpha;
        if(step==1)alpha=mask[y*w+x]&255;
        else {
            int p=y*2*w*2+x*2;
            alpha=((mask[p]&255)+(mask[p+1]&255)+(mask[p+w*2]&255)+
                    (mask[p+w*2+1]&255)+2)/4;
        }
        return 255-alpha;
    }
    // Normalized convolution excludes protected foreground from background blur.
    // Running column sums use O(width) scratch space, independent of blur radius.
    // U/V are processed separately and their order is never changed.
    private static void blurPlane(byte[] image,byte[] output,byte[] mask,int w,int h,
            int start,int step,int radius,BooleanSupplier cancelled) {
        int[] columnWeight=new int[w],columnSum=new int[w];
        for(int c=0;c<step;++c) {
            for(int x=0;x<w;++x) {
                if((x&255)==0)checkCancelled(cancelled);
                int weights=0,sum=0;
                for(int dy=-radius;dy<=radius;++dy) {
                    int sy=Math.max(0,Math.min(h-1,dy));
                    int weight=backgroundWeight(mask,w,x,sy,step);
                    weights+=weight;sum+=(image[start+(sy*w+x)*step+c]&255)*weight;
                }
                columnWeight[x]=weights;columnSum[x]=sum;
            }
            for(int y=0;y<h;++y) {
                checkCancelled(cancelled);
                int weights=0,sum=0;
                for(int dx=-radius;dx<=radius;++dx) {
                    int sx=Math.max(0,Math.min(w-1,dx));
                    weights+=columnWeight[sx];sum+=columnSum[sx];
                }
                for(int x=0;x<w;++x) {
                    int index=start+(y*w+x)*step+c;
                    output[index]=weights==0?image[index]:(byte)((sum+weights/2)/weights);
                    int remove=Math.max(0,x-radius),add=Math.min(w-1,x+radius+1);
                    weights+=columnWeight[add]-columnWeight[remove];
                    sum+=columnSum[add]-columnSum[remove];
                }
                int remove=Math.max(0,y-radius),add=Math.min(h-1,y+radius+1);
                for(int x=0;x<w;++x) {
                    int before=backgroundWeight(mask,w,x,remove,step);
                    int after=backgroundWeight(mask,w,x,add,step);
                    columnWeight[x]+=after-before;
                    columnSum[x]+=(image[start+(add*w+x)*step+c]&255)*after-
                            (image[start+(remove*w+x)*step+c]&255)*before;
                }
            }
        }
    }
    static byte[] process(byte[][] frames,int w,int h,Face[][] faces,boolean retouch,
            boolean details,boolean[] eligible,BooleanSupplier cancelled) {
        return process(frames,w,h,faces,retouch,details,eligible,100,cancelled);
    }
    static byte[] process(byte[][] frames,int w,int h,Face[][] faces,boolean retouch,
            boolean details,boolean[] eligible,int strength,BooleanSupplier cancelled) {
        checkCancelled(cancelled);
        long pixels=(long)w*h;
        if(w<32||h<32||(w&1)!=0||(h&1)!=0||pixels>MAX_PIXELS||frames==null||
                frames.length==0||frames.length>4)throw new IllegalArgumentException("Invalid burst");
        int size=(int)(pixels*3/2);
        for(byte[] f:frames)if(f==null||f.length!=size)throw new IllegalArgumentException("Invalid NV21");
        if((faces!=null&&faces.length!=frames.length)||(eligible!=null&&eligible.length!=frames.length))
            throw new IllegalArgumentException("Metadata count does not match images");
        checkStrength(strength);
        if(strength==0)return frames[0].clone();
        Offset[] offsets=new Offset[frames.length];
        offsets[0]=new Offset();offsets[0].usable=true;
        for(int i=1;i<frames.length;++i) {
            checkCancelled(cancelled);
            offsets[i]=align(frames[0],frames[i],w,h,cancelled);
            if(eligible!=null&&!eligible[i])offsets[i].usable=false;
        }
        byte[] output=frames[0].clone();
        if(frames.length>1||details)for(int y=1;y<h-1;++y) {
            checkCancelled(cancelled);
            for(int x=1;x<w-1;++x) {
            int base=value(frames[0],w,h,x,y),local=lowpass(frames[0],w,h,x,y);
            float sum=base*2,weights=2;
            for(int i=1;i<frames.length;++i) {
                Offset o=offsets[i];int sx=x+o.x,sy=y+o.y;
                if(!o.usable||sx<1||sy<1||sx>=w-1||sy>=h-1)continue;
                int v=value(frames[i],w,h,sx,sy),difference=Math.abs(local-lowpass(frames[i],w,h,sx,sy));
                if(difference>14||Math.abs(v-base)>40)continue;
                float weight=(14f-difference)/14;sum+=v*weight;weights+=weight;
            }
            float value=sum/weights,detail=base-local;
            if(details&&Math.abs(detail)>3&&Math.abs(detail)<28)value+=Math.max(-5,Math.min(5,detail*.3f));
            output[y*w+x]=clamp(value);
            }
        }
        if(frames.length>1)for(int y=0;y<h/2;++y) {
            checkCancelled(cancelled);
            for(int x=0;x<w;x+=2)for(int c=0;c<2;++c) {
            int index=w*h+y*w+x+c,base=frames[0][index]&255,sum=base*2,n=2;
            for(int i=1;i<frames.length;++i) {
                Offset o=offsets[i];int sx=x+Math.round(o.x/2f)*2,sy=y+Math.round(o.y/2f);
                if(!o.usable||sx<0||sy<0||sx>=w||sy>=h/2)continue;
                if(Math.abs(lowpass(frames[0],w,h,x,y*2)-lowpass(frames[i],w,h,x+o.x,y*2+o.y))>14)continue;
                int v=frames[i][w*h+sy*w+sx+c]&255;
                if(Math.abs(v-base)>24)continue;sum+=v;++n;
            }
            output[index]=(byte)((sum+n/2)/n);
            }
        }
        if(faces==null||faces[0]==null)return blendStrength(frames[0],output,strength,cancelled);
        // Overlapping detections must not apply the full effect repeatedly.
        byte[] applied=new byte[(int)pixels];
        for(int fi=0;fi<Math.min(MAX_FACES,faces[0].length);++fi) {
            Face face=faces[0][fi];
            checkCancelled(cancelled);
            if(!valid(face,w,h))continue;
            int donor=0;
            FaceQuality anchorQuality=quality(frames[0],w,h,face,0,0,cancelled);
            double best=anchorQuality.score();
            for(int i=1;i<frames.length;++i) {
                Offset o=offsets[i];if(!o.usable||!matching(face,faces[i],o,w,h))continue;
                FaceQuality q=quality(frames[i],w,h,face,o.x,o.y,cancelled);
                if(q.score()>best*1.15&&q.score()>best+.25&&
                        q.noise<=anchorQuality.noise*1.2+.5&&
                        q.clipped<=anchorQuality.clipped+.01&&
                        consistentFace(frames[0],frames[i],w,h,face,o,cancelled)) {
                    best=q.score();donor=i;
                }
            }
            FaceLight light=retouch?measureLight(frames[0],w,h,face,cancelled):null;
            for(int y=face.top;y<face.bottom;++y) {
                checkCancelled(cancelled);
                for(int x=face.left;x<face.right;++x) {
                int index=y*w+x;
                float fullAlpha=mask(face,x,y),alpha=Math.max(0,fullAlpha-(applied[index]&255)/255f);
                if(alpha<=0)continue;
                applied[index]=(byte)Math.round(fullAlpha*255);
                int anchor=value(frames[0],w,h,x,y),local=lowpass(frames[0],w,h,x,y);
                float v=output[index]&255;
                if(donor!=0) {
                    Offset o=offsets[donor];
                    int sample=value(frames[donor],w,h,x+o.x,y+o.y);
                    int donorLocal=lowpass(frames[donor],w,h,x+o.x,y+o.y);
                    if(Math.abs(local-donorLocal)<=5&&Math.abs(sample-anchor)<=12) {
                        float residual=(sample-donorLocal)-(anchor-local);
                        v+=Math.max(-6,Math.min(6,residual))*.25f*alpha;
                    }
                }
                if(retouch) {
                    float confidence=alpha*skin(frames[0],w,h,x,y)*featureProtection(face,x,y);
                    int gx=Math.abs(lowpass(frames[0],w,h,x+1,y)-lowpass(frames[0],w,h,x-1,y));
                    int gy=Math.abs(lowpass(frames[0],w,h,x,y+1)-lowpass(frames[0],w,h,x,y-1));
                    float structure=unit((22-gx-gy)/14f);
                    if(confidence>0) {
                        float sum=0,weights=0;
                        for(int dy=-2;dy<=2;++dy)for(int dx=-2;dx<=2;++dx) {
                            int p=value(frames[0],w,h,x+dx,y+dy);
                            float range=unit(1-Math.abs(p-anchor)/16f);
                            float weight=range*range/(1+dx*dx+dy*dy);
                            sum+=p*weight;weights+=weight;
                        }
                        // Bounded smoothing removes small variations while keeping
                        // most texture; large structures and facial features stay sharp.
                        float correction=Math.max(-4,Math.min(4,sum/weights-anchor));
                        v+=correction*.42f*confidence*structure;
                        v+=fillLight(light,face,x,local)*confidence;
                    }
                }
                output[index]=clamp(v);
                }
            }
        }
        checkCancelled(cancelled);
        return blendStrength(frames[0],output,strength,cancelled);
    }
    private static void checkStrength(int strength) {
        if(strength<0||strength>100)throw new IllegalArgumentException("Strength must be 0..100");
    }
    private static byte[] blendStrength(byte[] anchor,byte[] output,int strength,
            BooleanSupplier cancelled) {
        checkCancelled(cancelled);
        if(strength==100)return output;
        for(int i=0;i<output.length;++i) {
            if((i&4095)==0)checkCancelled(cancelled);
            output[i]=(byte)(((anchor[i]&255)*(100-strength)+(output[i]&255)*strength+50)/100);
        }
        checkCancelled(cancelled);return output;
    }
}
