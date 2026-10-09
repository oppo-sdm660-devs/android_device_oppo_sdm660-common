/* SPDX-License-Identifier: BSD-3-Clause */
package androidx.camera.extensions.impl;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import java.util.function.BooleanSupplier;

/** Bounded YUV delivery of the HAL's existing HDR/LLHDR JPEG composition. */
final class JpegYuvDecoder {
    private static int clamp(int value) { return Math.max(0,Math.min(255,value)); }
    static byte[] decode(byte[] jpeg,int width,int height,BooleanSupplier cancelled) {
        if(jpeg==null||width<32||height<32||(width&1)!=0||(height&1)!=0||
                (long)width*height>PixelPipeline.MAX_PIXELS)
            throw new IllegalArgumentException("Invalid HDR/NIGHT JPEG geometry");
        if(cancelled==null)cancelled=()->false;
        PixelPipeline.checkCancelled(cancelled);
        BitmapFactory.Options options=new BitmapFactory.Options();options.inJustDecodeBounds=true;
        BitmapFactory.decodeByteArray(jpeg,0,jpeg.length,options);
        if(options.outWidth!=width||options.outHeight!=height)
            throw new IllegalArgumentException("HAL JPEG dimensions disagree with capture");
        options.inJustDecodeBounds=false;options.inScaled=false;options.inPreferredConfig=Bitmap.Config.ARGB_8888;
        Bitmap bitmap=BitmapFactory.decodeByteArray(jpeg,0,jpeg.length,options);
        if(bitmap==null)throw new IllegalArgumentException("Invalid HAL JPEG");
        try {
            if(bitmap.getWidth()!=width||bitmap.getHeight()!=height)
                throw new IllegalArgumentException("Unexpected JPEG decode geometry");
            byte[] output=new byte[width*height*3/2];int[] rows=new int[width*2];
            for(int y=0;y<height;y+=2) {
                PixelPipeline.checkCancelled(cancelled);
                bitmap.getPixels(rows,0,width,0,y,width,2);
                for(int row=0;row<2;++row)for(int x=0;x<width;++x) {
                    int pixel=rows[row*width+x],r=(pixel>>16)&255,g=(pixel>>8)&255,b=pixel&255;
                    output[(y+row)*width+x]=(byte)clamp(((66*r+129*g+25*b+128)>>8)+16);
                }
                for(int x=0;x<width;x+=2) {
                    int r=0,g=0,b=0;
                    for(int row=0;row<2;++row)for(int dx=0;dx<2;++dx) {
                        int pixel=rows[row*width+x+dx];r+=(pixel>>16)&255;g+=(pixel>>8)&255;b+=pixel&255;
                    }
                    r=(r+2)/4;g=(g+2)/4;b=(b+2)/4;
                    int offset=width*height+(y/2)*width+x;
                    output[offset]=(byte)clamp(((112*r-94*g-18*b+128)>>8)+128);
                    output[offset+1]=(byte)clamp(((-38*r-74*g+112*b+128)>>8)+128);
                }
            }
            PixelPipeline.checkCancelled(cancelled);return output;
        } finally { bitmap.recycle(); }
    }
}
