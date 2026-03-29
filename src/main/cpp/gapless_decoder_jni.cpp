#include <jni.h>
#include "gapless_decoder.h"
#include <android/log.h>

#define LOG_TAG "NativeGaplessDecoder"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

extern "C" {

JNIEXPORT jlong JNICALL
Java_com_nerio_audioengine_NativeGaplessDecoder_nativeCreate(JNIEnv*, jclass,
        jint delay, jint padding, jint frameSize, jlong bufferHandle) {
    auto* buffer = reinterpret_cast<NativePcmBuffer*>(bufferHandle);
    if (!buffer) return 0;
    auto* decoder = new GaplessDecoder(delay, padding, frameSize, buffer);
    return reinterpret_cast<jlong>(decoder);
}

JNIEXPORT void JNICALL
Java_com_nerio_audioengine_NativeGaplessDecoder_nativeDestroy(JNIEnv*, jclass, jlong handle) {
    auto* decoder = reinterpret_cast<GaplessDecoder*>(handle);
    delete decoder;
}

JNIEXPORT jboolean JNICALL
Java_com_nerio_audioengine_NativeGaplessDecoder_nativeProcessFrame(JNIEnv* env, jclass,
        jlong handle, jbyteArray data, jint offset, jint length) {
    auto* decoder = reinterpret_cast<GaplessDecoder*>(handle);
    if (!decoder) return JNI_FALSE;

    jbyte* bytes = env->GetByteArrayElements(data, nullptr);
    if (!bytes) return JNI_FALSE;

    bool result = decoder->processFrame(reinterpret_cast<const uint8_t*>(bytes), offset, length);
    env->ReleaseByteArrayElements(data, bytes, JNI_ABORT);
    return result ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_nerio_audioengine_NativeGaplessDecoder_nativeSignalEnd(JNIEnv*, jclass, jlong handle) {
    auto* decoder = reinterpret_cast<GaplessDecoder*>(handle);
    if (decoder) decoder->signalEnd();
}

JNIEXPORT void JNICALL
Java_com_nerio_audioengine_NativeGaplessDecoder_nativeResetAfterSeek(JNIEnv*, jclass, jlong handle) {
    auto* decoder = reinterpret_cast<GaplessDecoder*>(handle);
    if (decoder) decoder->resetAfterSeek();
}

} // extern "C"
