/* SPDX-License-Identifier: BSD-3-Clause */
package androidx.camera.extensions.impl;

import java.util.Arrays;
import java.util.function.BooleanSupplier;

/** Explicit, bounded photo edits. Geometry uses caller anchors, coarse face
 * rectangles or observed FULL landmarks; it does not estimate missing points.
 * Coordinates for decorations are relative to the final, optionally mirrored,
 * unrotated output. The metadata/crop/timestamp anchor is never changed here. */
final class PortraitPostprocess {
    static final int MAX_OVERLAY_SIDE=32,MAX_TEXT=32,SETTINGS_COUNT=10,DECORATION_SCALE=10000;
    static final int[] DEFAULTS={1,0,0,0,0,0,0,0,0xffffffff,1};
    static final class Options {
        final int[] settings,overlay,tracking,layout;final float[] warps;final byte[] text;
        Options(int[] settings,float[] warps,int[] overlay,byte[] text) {
            this(settings,warps,overlay,text,null,null);
        }
        Options(int[] settings,float[] warps,int[] overlay,byte[] text,int[] tracking) {
            this(settings,warps,overlay,text,tracking,null);
        }
        Options(int[] settings,float[] warps,int[] overlay,byte[] text,int[] tracking,int[] layout) {
            if((settings!=null&&settings.length!=SETTINGS_COUNT)||(warps!=null&&warps.length>24)||
                    (overlay!=null&&overlay.length>6+MAX_OVERLAY_SIDE*MAX_OVERLAY_SIDE)||
                    (text!=null&&text.length>MAX_TEXT)||(tracking!=null&&tracking.length!=5)||
                    (layout!=null&&layout.length!=4))throw new IllegalArgumentException("Postprocess payload exceeds bounds");
            this.settings=settings==null?DEFAULTS.clone():settings.clone();
            this.warps=warps==null?new float[0]:warps.clone();
            this.overlay=overlay==null?new int[0]:overlay.clone();
            this.text=text==null?new byte[0]:text.clone();
            this.tracking=tracking==null?new int[]{1,0,0,0,0}:tracking.clone();
            this.layout=layout==null?null:layout.clone();
            if(this.tracking[0]!=1||Math.abs((long)this.tracking[1])>100||Math.abs((long)this.tracking[2])>100||
                this.tracking[3]<0||this.tracking[3]>1||this.tracking[4]<0||this.tracking[4]>1)
                throw new IllegalArgumentException("Invalid faceTracking v1 payload");
            validate();
        }
        private void validate() {
            if(settings.length!=SETTINGS_COUNT||settings[0]!=1||settings[1]<0||settings[1]>100||
                    Math.abs((long)settings[2])>100||Math.abs((long)settings[3])>100||
                    Math.abs((long)settings[4])>45||settings[5]<0||settings[5]>100||
                    settings[6]<0||settings[6]>1||settings[7]<0||settings[7]>3||
                    settings[9]<1||settings[9]>4)throw new IllegalArgumentException("Invalid postprocess v1 settings");
            if(warps.length%6!=0||warps.length>24)throw new IllegalArgumentException("At most four warp regions");
            for(int i=0;i<warps.length;i+=6) {
                for(int j=0;j<6;++j)if(!Float.isFinite(warps[i+j]))throw new IllegalArgumentException("Non-finite warp");
                float x=warps[i],y=warps[i+1],rx=warps[i+2],ry=warps[i+3];
                if(rx<.01f||rx>.25f||ry<.01f||ry>.25f||x-rx<0||x+rx>1||y-ry<0||y+ry>1||
                        warps[i+4]<.85f||warps[i+4]>1.15f||warps[i+5]<.85f||warps[i+5]>1.15f)
                    throw new IllegalArgumentException("Warp geometry outside bounded output region");
            }
            if(overlay.length!=0) {
                if(overlay.length<6||overlay[0]<0||overlay[1]<0||overlay[2]<1||overlay[3]<1||
                        overlay[0]+(long)overlay[2]>10000||overlay[1]+(long)overlay[3]>10000||
                        overlay[2]>5000||overlay[3]>5000||overlay[4]<1||overlay[4]>MAX_OVERLAY_SIDE||
                        overlay[5]<1||overlay[5]>MAX_OVERLAY_SIDE||overlay.length!=6+overlay[4]*overlay[5])
                    throw new IllegalArgumentException("Invalid bounded ARGB overlay");
            }
            if(text.length>MAX_TEXT)throw new IllegalArgumentException("Watermark exceeds 32 characters");
            for(byte b:text)if(glyph((char)(b&255))==null)throw new IllegalArgumentException("Unsupported watermark character");
            if(layout!=null) {
                // Ten-thousandths of the final output, matching the overlay
                // schema. The box only places existing decorations.
                if(layout[0]<0||layout[1]<0||layout[2]>DECORATION_SCALE||layout[3]>DECORATION_SCALE||
                        layout[0]>=layout[2]||layout[1]>=layout[3]||
                        (overlay.length==0&&text.length==0))
                    throw new IllegalArgumentException("Invalid bounded watermark layout");
            }
        }
        boolean active() {
            if(settings[1]!=0||settings[2]!=0||settings[3]!=0||settings[4]!=0||settings[5]!=0||settings[6]!=0||
                    overlay.length>0||text.length>0||tracking[1]!=0||tracking[2]!=0||tracking[3]!=0||tracking[4]!=0)return true;
            // A layout alone has nothing to place and stays inactive.
            for(int i=0;i<warps.length;i+=6)if(warps[i+4]!=1||warps[i+5]!=1)return true;
            return false;
        }
    }
    private static int clip(int value,int high){return Math.max(0,Math.min(high,value));}
    private static int byteValue(float value){return clip(Math.round(value),255);}
    private static float unit(float value){return Math.max(0,Math.min(1,value));}
    private static boolean valid(PixelPipeline.Face f,int w,int h) {
        return f!=null&&f.left>=0&&f.top>=0&&f.right<=w&&f.bottom<=h&&f.right-f.left>=8&&f.bottom-f.top>=8;
    }
    static byte[] process(byte[] image,int w,int h,PixelPipeline.Face[] faces,Options options,
            int strength,BooleanSupplier cancelled) {
        PixelPipeline.checkCancelled(cancelled);
        if(image==null||w<32||h<32||(w&1)!=0||(h&1)!=0||(long)w*h>PixelPipeline.MAX_PIXELS||
                image.length!=(long)w*h*3/2||strength<0||strength>100||options==null)
            throw new IllegalArgumentException("Invalid postprocess image");
        byte[] output=image.clone();
        if(strength==0||!options.active())return output;
        if(options.settings[1]>0||options.warps.length>0||options.tracking[1]!=0||options.tracking[2]!=0)
            output=warp(image,w,h,faces,options,strength,cancelled);
        color(output,w,h,options,strength,cancelled);
        // Mirror is a discrete, explicitly requested geometry operation. Its
        // enable is gated by nonzero extension strength, not fractional blending.
        if(options.settings[6]!=0)mirror(output,w,h,cancelled);
        decorate(output,w,h,faces,options,strength,cancelled);
        PixelPipeline.checkCancelled(cancelled);return output;
    }
    private static float sample(byte[] image,int w,int h,int start,int step,int c,float x,float y) {
        x=Math.max(0,Math.min(w-1,x));y=Math.max(0,Math.min(h-1,y));
        int ix=(int)x,iy=(int)y,jx=Math.min(w-1,ix+1),jy=Math.min(h-1,iy+1);
        float fx=x-ix,fy=y-iy;
        return (image[start+(iy*w+ix)*step+c]&255)*(1-fx)*(1-fy)+
                (image[start+(iy*w+jx)*step+c]&255)*fx*(1-fy)+
                (image[start+(jy*w+ix)*step+c]&255)*(1-fx)*fy+
                (image[start+(jy*w+jx)*step+c]&255)*fx*fy;
    }
    private static void coordinate(float[] p,float x,float y,int w,int h,PixelPipeline.Face[] faces,
            Options options,float amount) {
        float sx=x,sy=y,best=0,slim=0;
        if(options.settings[1]>0&&faces!=null)for(int i=0;i<Math.min(10,faces.length);++i) {
            PixelPipeline.Face f=faces[i];if(!valid(f,w,h))continue;
            float nx=(2*x-f.left-f.right)/(f.right-f.left),ny=(y-f.top)/(f.bottom-f.top);
            float weight=unit((1-nx*nx)*2)*unit((ny-.30f)/.25f)*unit((1-ny)/.20f);
            if(weight>best){best=weight;slim=(x-(f.left+f.right)*.5f)*weight;}
        }
        // Inverse mapping pulls a lower-face silhouette inward by <=4% of
        // half-width. It is an explicit coarse edit, never an anatomical model.
        sx+=slim*options.settings[1]*.0004f*amount;
        if(faces!=null&&(options.tracking[1]!=0||options.tracking[2]!=0))for(int i=0;i<Math.min(10,faces.length);++i) {
            PixelPipeline.Face f=faces[i];if(f==null||!f.hasLandmarks()||x<f.left||x>=f.right||y<f.top||y>=f.bottom)continue;
            float d=(float)Math.hypot(f.landmarks[2]-f.landmarks[0],f.landmarks[3]-f.landmarks[1]);
            for(int part=0;part<3;++part) {
                int setting=options.tracking[part==2?2:1];if(setting==0)continue;
                float cx=f.landmarks[part*2],cy=f.landmarks[part*2+1],rx=d*(part==2?.30f:.23f),ry=rx*.70f;
                float dx=x-cx,dy=y-cy,r=dx*dx/(rx*rx)+dy*dy/(ry*ry);if(r>=1)continue;
                float weight=(1-r)*(1-r)*amount;
                float scale=1+setting*(part==2?.0012f:.001f);
                sx+=dx*(1/scale-1)*weight;sy+=dy*(1/scale-1)*weight;
            }
        }
        for(int i=0;i<options.warps.length;i+=6) {
            float cx=options.warps[i]*w,cy=options.warps[i+1]*h;
            float rx=options.warps[i+2]*w,ry=options.warps[i+3]*h;
            float dx=x-cx,dy=y-cy,d=dx*dx/(rx*rx)+dy*dy/(ry*ry);
            if(d>=1)continue;
            float weight=(1-d)*(1-d)*amount;
            sx+=dx*(1/options.warps[i+4]-1)*weight;
            sy+=dy*(1/options.warps[i+5]-1)*weight;
        }
        // Even overlapping caller regions cannot create an unbounded displacement.
        p[0]=x+Math.max(-w*.04f,Math.min(w*.04f,sx-x));
        p[1]=y+Math.max(-h*.04f,Math.min(h*.04f,sy-y));
    }
    private static byte[] warp(byte[] image,int w,int h,PixelPipeline.Face[] faces,Options options,
            int strength,BooleanSupplier cancelled) {
        byte[] output=image.clone();float[] p=new float[2];float amount=strength/100f;
        for(int y=0;y<h;++y) {
            PixelPipeline.checkCancelled(cancelled);
            for(int x=0;x<w;++x) {
                coordinate(p,x,y,w,h,faces,options,amount);
                output[y*w+x]=(byte)byteValue(sample(image,w,h,0,1,0,p[0],p[1]));
            }
        }
        for(int y=0;y<h/2;++y) {
            PixelPipeline.checkCancelled(cancelled);
            for(int x=0;x<w/2;++x) {
                coordinate(p,x*2+.5f,y*2+.5f,w,h,faces,options,amount);
                for(int c=0;c<2;++c)output[w*h+y*w+x*2+c]=(byte)byteValue(
                        sample(image,w/2,h/2,w*h,2,c,(p[0]-.5f)/2,(p[1]-.5f)/2));
            }
        }
        return output;
    }
    private static void color(byte[] image,int w,int h,Options options,int strength,BooleanSupplier cancelled) {
        int[] s=options.settings;float amount=strength/100f;
        if(s[2]==0&&s[3]==0&&s[4]==0&&s[5]==0)return;
        int[] histogram=new int[256];int count=0;
        for(int y=0;y<h;y+=4) {
            PixelPipeline.checkCancelled(cancelled);
            for(int x=0;x<w;x+=4){++histogram[image[y*w+x]&255];++count;}
        }
        int low=percentile(histogram,count,10),high=percentile(histogram,count,90);
        // Measured low-contrast compensation only. No scene label is inferred.
        float scene=unit((160-(high-low))/160f)*(s[5]/100f)*amount;
        float contrast=1+s[2]/200f*amount+scene*.12f;
        float saturation=Math.max(0,1+s[3]/100f*amount+scene*.10f);
        double angle=Math.toRadians(s[4]*amount);float cos=(float)Math.cos(angle),sin=(float)Math.sin(angle);
        for(int y=0;y<h;++y) {
            PixelPipeline.checkCancelled(cancelled);
            for(int x=0;x<w;++x) {
                int index=y*w+x,v=image[index]&255;
                float edited=128+(v-128)*contrast;
                // Blend the change down toward the luma extremes to protect them.
                float protect=unit(v/24f)*unit((255-v)/24f);
                image[index]=(byte)byteValue(v+(edited-v)*protect);
            }
        }
        for(int y=0;y<h/2;++y) {
            PixelPipeline.checkCancelled(cancelled);
            for(int x=0;x<w;x+=2) {
                int p=w*h+y*w+x;float v=(image[p]&255)-128,u=(image[p+1]&255)-128;
                image[p]=(byte)byteValue(128+(v*cos-u*sin)*saturation);
                image[p+1]=(byte)byteValue(128+(u*cos+v*sin)*saturation);
            }
        }
    }
    private static int percentile(int[] histogram,int n,int percent) {
        int target=(n*percent+99)/100,total=0;
        for(int i=0;i<256;++i)if((total+=histogram[i])>=target)return i;return 255;
    }
    private static void mirror(byte[] image,int w,int h,BooleanSupplier cancelled) {
        for(int y=0;y<h;++y) {
            PixelPipeline.checkCancelled(cancelled);
            for(int x=0;x<w/2;++x){int a=y*w+x,b=y*w+w-1-x;byte swap=image[a];image[a]=image[b];image[b]=swap;}
        }
        for(int y=0;y<h/2;++y) {
            PixelPipeline.checkCancelled(cancelled);
            for(int x=0;x<w/2;x+=2)for(int c=0;c<2;++c) {
                int a=w*h+y*w+x+c,b=w*h+y*w+w-2-x+c;byte swap=image[a];image[a]=image[b];image[b]=swap;
            }
        }
    }
    private static int trackedOverlay(int x,int y,PixelPipeline.Face[] faces,int[] overlay,boolean mirrored,int w) {
        if(faces==null)return 0;
        for(int i=0;i<Math.min(10,faces.length);++i) {
            PixelPipeline.Face f=faces[i];if(f==null||!f.hasLandmarks())continue;
            // Undo the optional final mirror before mapping actual three-point
            // geometry. The texture is mirrored with the face as one unit.
            float px=mirrored?w-1-x:x,py=y;
            if(px<f.left||px>=f.right||py<f.top||py>=f.bottom)continue;
            float ax=(f.landmarks[2]-f.landmarks[0])/.4f,ay=(f.landmarks[3]-f.landmarks[1])/.4f;
            float bx=(f.landmarks[4]-(f.landmarks[0]+f.landmarks[2])*.5f)/.4f;
            float by=(f.landmarks[5]-(f.landmarks[1]+f.landmarks[3])*.5f)/.4f;
            float ox=f.landmarks[0]-.3f*ax-.35f*bx,oy=f.landmarks[1]-.3f*ay-.35f*by;
            float det=ax*by-ay*bx;if(Math.abs(det)<1)continue;
            float u=((px-ox)*by-(py-oy)*bx)/det,v=(ax*(py-oy)-ay*(px-ox))/det;
            if(u>=0&&u<1&&v>=0&&v<1)return overlay[6+(int)(v*overlay[5])*overlay[4]+(int)(u*overlay[4])];
        }
        return 0;
    }
    private static void decorate(byte[] image,int w,int h,PixelPipeline.Face[] faces,Options options,int strength,BooleanSupplier cancelled) {
        if(options.overlay.length==0&&options.text.length==0)return;
        // Overlay and text are rasterized per 2x2 NV21 chroma block. The alpha of
        // each luma sample is independent; chroma uses all four alphas once.
        int[] argb=new int[4];int[] overlay=options.overlay,s=options.settings;
        int scale=Math.max(1,Math.min(4,s[9]*Math.max(1,Math.min(w,h)/480)));
        int textWidth=options.text.length*6*scale,textHeight=7*scale,margin=Math.max(2,Math.min(w,h)/80);
        int tx=(s[7]&1)==0?w-margin-textWidth:margin;
        int ty=(s[7]&2)==0?h-margin-textHeight:margin;
        int ox=overlay.length==0?0:(int)((long)overlay[0]*w/10000),oy=overlay.length==0?0:(int)((long)overlay[1]*h/10000);
        int ow=overlay.length==0?0:Math.max(1,(int)((long)overlay[2]*w/10000));
        int oh=overlay.length==0?0:Math.max(1,(int)((long)overlay[3]*h/10000));
        if(options.layout!=null) {
            // An explicit watermark layout replaces the corner anchor of the
            // text and scales the overlay into the requested box.
            ox=(int)((long)options.layout[0]*w/DECORATION_SCALE);
            oy=(int)((long)options.layout[1]*h/DECORATION_SCALE);
            ow=Math.max(1,(int)((long)(options.layout[2]-options.layout[0])*w/DECORATION_SCALE));
            oh=Math.max(1,(int)((long)(options.layout[3]-options.layout[1])*h/DECORATION_SCALE));
            if(overlay.length==0){ow=0;oh=0;}
            tx=Math.min(Math.max(0,w-1),ox);ty=Math.min(Math.max(0,h-1),oy);
        }
        for(int y=0;y<h;y+=2) {
            PixelPipeline.checkCancelled(cancelled);
            for(int x=0;x<w;x+=2) {
                Arrays.fill(argb,0);
                for(int dy=0;dy<2;++dy)for(int dx=0;dx<2;++dx) {
                    int px=x+dx,py=y+dy,color=0;
                    if(overlay.length>0) {
                        if(options.tracking[3]!=0)color=trackedOverlay(px,py,faces,overlay,s[6]!=0,w);
                        else if(px>=ox&&py>=oy&&px<ox+ow&&py<oy+oh) {
                            int sx=(px-ox)*overlay[4]/ow,sy=(py-oy)*overlay[5]/oh;
                            color=overlay[6+sy*overlay[4]+sx];
                        }
                    }
                    int lx=px-tx,ly=py-ty;
                    if(lx>=0&&ly>=0&&lx<textWidth&&ly<textHeight&&options.text.length>0) {
                        int column=lx/scale,letter=column/6,bit=column%6,row=ly/scale;
                        String glyph=glyph((char)(options.text[letter]&255));
                        if(bit<5&&glyph.charAt(row*5+bit)=='1')color=over(color,s[8]);
                    }
                    argb[dy*2+dx]=color;
                }
                int alpha=0,vu=0,uu=0;
                for(int i=0;i<4;++i) {
                    int color=argb[i],a=(color>>>24)*strength/100;
                    int r=(color>>>16)&255,g=(color>>>8)&255,b=color&255;
                    int yy=clip(((66*r+129*g+25*b+128)>>8)+16,255);
                    int v=clip(((112*r-94*g-18*b+128)>>8)+128,255),u=clip(((-38*r-74*g+112*b+128)>>8)+128,255);
                    int p=(y+i/2)*w+x+i%2,old=image[p]&255;
                    image[p]=(byte)((old*(255-a)+yy*a+127)/255);alpha+=a;vu+=v*a;uu+=u*a;
                }
                int p=w*h+(y/2)*w+x;
                image[p]=(byte)(((image[p]&255)*(1020-alpha)+vu+510)/1020);
                image[p+1]=(byte)(((image[p+1]&255)*(1020-alpha)+uu+510)/1020);
            }
        }
    }
    private static int over(int bottom,int top) {
        int a=top>>>24,b=bottom>>>24,out=a+(b*(255-a)+127)/255;
        if(out==0)return 0;int rgb=0;
        for(int shift=0;shift<=16;shift+=8) {
            int premult=((top>>>shift)&255)*a+((bottom>>>shift)&255)*b*(255-a)/255;
            rgb|=clip((premult+out/2)/out,255)<<shift;
        }
        return (out<<24)|rgb;
    }
    // A small deterministic 5x7 font. Supported input is uppercase A-Z, digits,
    // space, period, colon, slash, underscore and hyphen; no invisible substitution.
    private static final String ALPHABET=" ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.:-_/";
    private static final String[] FONT={
        "00000000000000000000000000000000000","01110100011000111111100011000110001","11110100011000111110100011000111110",
        "01111100001000010000100001000001111","11110100011000110001100011000111110","11111100001000011110100001000011111",
        "11111100001000011110100001000010000","01111100001000010111100011000101111","10001100011000111111100011000110001",
        "11111001000010000100001000010011111","00111000100001000010100101001001100","10001100101010011000101001001010001",
        "10000100001000010000100001000011111","10001110111010110101100011000110001","10001110011010110011100011000110001",
        "01110100011000110001100011000101110","11110100011000111110100001000010000","01110100011000110001101011001001101",
        "11110100011000111110101001001010001","01111100001000001110000010000111110","11111001000010000100001000010000100",
        "10001100011000110001100011000101110","10001100011000110001100010101000100","10001100011000110101101011101110001",
        "10001100010101000100010101000110001","10001100010101000100001000010000100","11111000010001000100010001000011111",
        "01110100011001110101110011000101110","00100011000010000100001000010001110","01110100010000100010001000100011111",
        "11110000010000101110000010000111110","00010001100101010010111110001000010","11111100001000011110000010000111110",
        "01110100001000011110100011000101110","11111000010001000100010000100001000","01110100011000101110100011000101110",
        "01110100011000101111000010000101110","00000000000000000000000000011000110","00000001100011000000001100011000000",
        "00000000000000011111000000000000000","00000000000000000000000000000011111","00001000100001000100010001000010000"};
    private static String glyph(char letter) {
        int index=ALPHABET.indexOf(letter);if(index<0)return null;
        return FONT[index];
    }
}
