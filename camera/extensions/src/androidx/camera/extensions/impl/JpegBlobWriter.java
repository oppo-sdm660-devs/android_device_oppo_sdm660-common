/* SPDX-License-Identifier: BSD-3-Clause */
package androidx.camera.extensions.impl;

import android.media.Image;

/** Writes an ImageWriter BLOB using its allocation, including recycled buffers.
 * Android's JPEG plane view can end at the previous JPEG length. Its native
 * mapping still covers Image.getWidth() bytes; see AOSP Image_getBlobSize and
 * the advanced Extensions JpegEncoder. Never expose this API for arbitrary
 * ByteBuffers: only a dequeued JPEG ImageWriter image owns that full mapping.
 */
final class JpegBlobWriter {
    private static final boolean AVAILABLE;
    static {
        boolean loaded=false;
        try { System.loadLibrary("oppo_camera_extensions_jni");loaded=true; }
        catch(UnsatisfiedLinkError failure) { System.err.println("Camera Extensions JPEG writer unavailable: "+failure); }
        AVAILABLE=loaded;
    }
    static boolean available() { return AVAILABLE; }
    static void write(byte[] jpeg,Image image,int allocation) {
        if(!AVAILABLE)throw new IllegalStateException("JPEG writer library not installed");
        writeNative(jpeg,image,allocation);
    }
    private static native void writeNative(byte[] jpeg,Image image,int allocation);
}
