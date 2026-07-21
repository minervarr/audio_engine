#include <jni.h>
#include "pcm_buffer.h"
#include <android/log.h>

#define LOG_TAG "NativePcmBuffer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

extern "C" {

JNIEXPORT jlong JNICALL
Java_com_nerio_audioengine_NativePcmBuffer_nativeCreate(JNIEnv*, jclass, jint capacity) {
    auto* buf = new NativePcmBuffer((size_t)capacity);
    return reinterpret_cast<jlong>(buf);
}

JNIEXPORT void JNICALL
Java_com_nerio_audioengine_NativePcmBuffer_nativeDestroy(JNIEnv*, jclass, jlong handle) {
    auto* buf = reinterpret_cast<NativePcmBuffer*>(handle);
    delete buf;
}

JNIEXPORT jboolean JNICALL
Java_com_nerio_audioengine_NativePcmBuffer_nativeWrite(JNIEnv* env, jclass,
        jlong handle, jbyteArray data, jint offset, jint length) {
    auto* buf = reinterpret_cast<NativePcmBuffer*>(handle);
    if (!buf) return JNI_FALSE;

    jbyte* bytes = env->GetByteArrayElements(data, nullptr);
    if (!bytes) return JNI_FALSE;

    bool result = buf->write(reinterpret_cast<const uint8_t*>(bytes), offset, length);
    env->ReleaseByteArrayElements(data, bytes, JNI_ABORT);
    return result ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_com_nerio_audioengine_NativePcmBuffer_nativeRead(JNIEnv* env, jclass,
        jlong handle, jbyteArray dest, jint offset, jint maxLength) {
    auto* buf = reinterpret_cast<NativePcmBuffer*>(handle);
    if (!buf) return -1;

    jbyte* bytes = env->GetByteArrayElements(dest, nullptr);
    if (!bytes) return -1;

    int result = buf->read(reinterpret_cast<uint8_t*>(bytes), offset, maxLength);
    env->ReleaseByteArrayElements(dest, bytes, 0); // copy back
    return result;
}

JNIEXPORT void JNICALL
Java_com_nerio_audioengine_NativePcmBuffer_nativeFlush(JNIEnv*, jclass, jlong handle) {
    auto* buf = reinterpret_cast<NativePcmBuffer*>(handle);
    if (buf) buf->flush();
}

JNIEXPORT void JNICALL
Java_com_nerio_audioengine_NativePcmBuffer_nativeSignalEnd(JNIEnv*, jclass, jlong handle) {
    auto* buf = reinterpret_cast<NativePcmBuffer*>(handle);
    if (buf) buf->signalEnd();
}

JNIEXPORT void JNICALL
Java_com_nerio_audioengine_NativePcmBuffer_nativeReset(JNIEnv*, jclass, jlong handle) {
    auto* buf = reinterpret_cast<NativePcmBuffer*>(handle);
    if (buf) buf->reset();
}

} // extern "C"
