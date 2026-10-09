/* SPDX-License-Identifier: BSD-3-Clause */
package androidx.camera.extensions.impl;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;

/** camera3_jpeg_blob_t trailer plus a minimal standard EXIF IFD. The IFD always
 * carries the orientation tag and, when the caller requested one, the standard
 * EXIF UserComment (tag 0x9286, UNDEFINED with an "ASCII" character code). */
final class JpegBlob {
    private static final int MAX_COMMENT=256;
    static byte[] withOrientation(byte[] jpeg,int rotation) {
        return withOrientation(jpeg,rotation,null);
    }
    static byte[] withOrientation(byte[] jpeg,int rotation,byte[] comment) {
        if(jpeg==null||jpeg.length<4||(jpeg[0]&255)!=255||(jpeg[1]&255)!=216||
                (jpeg[jpeg.length-2]&255)!=255||(jpeg[jpeg.length-1]&255)!=217||rotation%90!=0)
            throw new IllegalArgumentException("Invalid JPEG or orientation");
        int angle=((rotation%360)+360)%360;
        int orientation=angle==90?6:angle==180?3:angle==270?8:1;
        byte[] text=userComment(comment);
        // IFD0 with the orientation entry and an optional UserComment entry.
        // Tags stay sorted (0x0112 < 0x9286) as the EXIF/TIFF format requires.
        int entries=text.length==0?1:2;
        int ifdSize=2+entries*12+4;
        int commentSize=text.length==0?0:8+text.length;
        int segmentSize=2+6+8+ifdSize+commentSize;
        byte[] app1=new byte[2+segmentSize];
        int p=0;
        app1[p++]=(byte)255;app1[p++]=(byte)225;
        app1[p++]=(byte)((segmentSize>>8)&255);app1[p++]=(byte)(segmentSize&255);
        app1[p++]='E';app1[p++]='x';app1[p++]='i';app1[p++]='f';app1[p++]=0;app1[p++]=0;
        app1[p++]='I';app1[p++]='I';app1[p++]=42;app1[p++]=0;
        app1[p++]=8;app1[p++]=0;app1[p++]=0;app1[p++]=0;      // IFD0 offset
        app1[p++]=(byte)(entries&255);app1[p++]=(byte)((entries>>8)&255);
        p=entry(app1,p,0x0112,3,1,orientation);
        if(text.length!=0)p=entry(app1,p,0x9286,7,commentSize,8+ifdSize);
        app1[p++]=0;app1[p++]=0;app1[p++]=0;app1[p++]=0;      // no next IFD
        if(text.length!=0) {
            app1[p++]='A';app1[p++]='S';app1[p++]='C';app1[p++]='I';app1[p++]=0;app1[p++]=0;app1[p++]=0;app1[p++]=0;
            System.arraycopy(text,0,app1,p,text.length);
            p+=text.length;
        }
        if(p!=app1.length)throw new IllegalStateException("EXIF segment size mismatch");
        byte[] out=new byte[jpeg.length+app1.length];
        System.arraycopy(jpeg,0,out,0,2);System.arraycopy(app1,0,out,2,app1.length);
        System.arraycopy(jpeg,2,out,2+app1.length,jpeg.length-2);return out;
    }
    private static int entry(byte[] out,int p,int tag,int type,int count,int value) {
        out[p++]=(byte)(tag&255);out[p++]=(byte)((tag>>8)&255);
        out[p++]=(byte)(type&255);out[p++]=(byte)((type>>8)&255);
        out[p++]=(byte)(count&255);out[p++]=(byte)((count>>8)&255);out[p++]=0;out[p++]=0;
        out[p++]=(byte)(value&255);out[p++]=(byte)((value>>8)&255);
        out[p++]=(byte)((value>>16)&255);out[p++]=(byte)((value>>24)&255);
        return p;
    }
    /** Validated UTF-8 payload, or empty when no comment was requested. */
    private static byte[] userComment(byte[] comment) {
        if(comment==null||comment.length==0)return new byte[0];
        if(comment.length>MAX_COMMENT)throw new IllegalArgumentException("EXIF comment exceeds 256 bytes");
        for(byte value:comment)if(value==0)throw new IllegalArgumentException("EXIF comment must not contain NUL");
        try {
            StandardCharsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(comment));
        } catch(CharacterCodingException failure) {
            throw new IllegalArgumentException("EXIF comment is not valid UTF-8");
        }
        return comment.clone();
    }
    static void write(byte[] jpeg,ByteBuffer plane,int allocation) {
        ByteBuffer out=plane.duplicate().order(ByteOrder.LITTLE_ENDIAN);
        // ImageWriter can expose the *previous JPEG length* when it recycles a
        // BLOB. That short view does not include the real allocation trailer;
        // writing a trailer at its limit would silently corrupt the contract.
        if(jpeg==null||out.isReadOnly()||allocation<8||out.position()!=0||out.limit()!=allocation||
                jpeg.length>allocation-8)
            throw new IllegalArgumentException("JPEG requires a complete BLOB allocation");
        out.put(jpeg);int trailer=allocation-8;
        out.putShort(trailer,(short)0xff);out.putShort(trailer+2,(short)0);out.putInt(trailer+4,jpeg.length);
    }
}
