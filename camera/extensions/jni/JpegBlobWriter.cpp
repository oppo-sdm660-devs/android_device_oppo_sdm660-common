/* SPDX-License-Identifier: BSD-3-Clause */
#include <jni.h>
#include <stdint.h>
#include <string.h>

namespace {
void reject(JNIEnv* env, const char* message) {
    jclass type=env->FindClass("java/lang/IllegalArgumentException");
    if(type)env->ThrowNew(type,message);
}
}

extern "C" JNIEXPORT void JNICALL
Java_androidx_camera_extensions_impl_JpegBlobWriter_writeNative(
        JNIEnv* env,jclass,jbyteArray jpeg,jobject image,jint allocation) {
    if(!jpeg||!image||allocation<8) { reject(env,"Missing JPEG allocation");return; }
    const jsize length=env->GetArrayLength(jpeg);
    if(length<4||length>allocation-8) { reject(env,"JPEG exceeds BLOB allocation");return; }
    jclass imageType=env->FindClass("android/media/Image");
    if(!imageType)return;
    jmethodID width=env->GetMethodID(imageType,"getWidth","()I");
    jmethodID height=env->GetMethodID(imageType,"getHeight","()I");
    jmethodID format=env->GetMethodID(imageType,"getFormat","()I");
    jmethodID getPlanes=env->GetMethodID(imageType,"getPlanes","()[Landroid/media/Image$Plane;");
    if(!width||!height||!format||!getPlanes)return;
    const jint w=env->CallIntMethod(image,width),h=env->CallIntMethod(image,height);
    const jint f=env->CallIntMethod(image,format);
    if(env->ExceptionCheck())return;
    if(w!=allocation||h!=1||f!=256) { reject(env,"Expected an ImageWriter JPEG BLOB");return; }
    auto planes=static_cast<jobjectArray>(env->CallObjectMethod(image,getPlanes));
    if(env->ExceptionCheck())return;
    if(!planes||env->GetArrayLength(planes)!=1) { reject(env,"Expected one JPEG plane");return; }
    jobject plane=env->GetObjectArrayElement(planes,0);
    if(env->ExceptionCheck())return;
    if(!plane) { reject(env,"Missing JPEG plane");return; }
    jclass planeType=env->FindClass("android/media/Image$Plane");
    if(!planeType)return;
    jmethodID getBuffer=env->GetMethodID(planeType,"getBuffer","()Ljava/nio/ByteBuffer;");
    if(!getBuffer)return;
    jobject buffer=env->CallObjectMethod(plane,getBuffer);
    if(env->ExceptionCheck())return;
    if(!buffer) { reject(env,"Missing JPEG buffer");return; }
    jclass bufferType=env->FindClass("java/nio/ByteBuffer");
    if(!bufferType)return;
    jmethodID position=env->GetMethodID(bufferType,"position","()I");
    jmethodID readOnly=env->GetMethodID(bufferType,"isReadOnly","()Z");
    if(!position||!readOnly)return;
    const jint offset=env->CallIntMethod(buffer,position);
    const jboolean immutable=env->CallBooleanMethod(buffer,readOnly);
    if(env->ExceptionCheck())return;
    auto data=static_cast<uint8_t*>(env->GetDirectBufferAddress(buffer));
    const jlong view=env->GetDirectBufferCapacity(buffer);
    // getPlanes() locks the complete gralloc buffer and consumes its fence.
    // Only the JNI ByteBuffer *view* is shortened by an old JPEG trailer.
    // ImageWriter's BLOB native width is the real allocation (AOSP JNI).
    if(!data||offset!=0||immutable||view<4||view>allocation) {
        reject(env,"Invalid writable ImageWriter JPEG mapping");return;
    }
    env->GetByteArrayRegion(jpeg,0,length,reinterpret_cast<jbyte*>(data));
    if(env->ExceptionCheck())return;
    // AIDL CameraBlob and legacy camera3_jpeg_blob_t are both eight bytes.
    // Use explicit bytes for an unaligned trailer and either ARM ABI.
    const uint8_t trailer[8]={0xff,0,0,0,static_cast<uint8_t>(length),
        static_cast<uint8_t>(length>>8),static_cast<uint8_t>(length>>16),static_cast<uint8_t>(length>>24)};
    memcpy(data+allocation-8,trailer,sizeof(trailer));
}
