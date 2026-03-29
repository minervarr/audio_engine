#include <jni.h>
#include "eq_processor.h"
#include <android/log.h>

#define LOG_TAG "EqProcessor"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

extern "C" {

JNIEXPORT jlong JNICALL
Java_com_nerio_audioengine_EqProcessor_nativeCreate(JNIEnv*, jclass) {
    auto* proc = new EqProcessor();
    return reinterpret_cast<jlong>(proc);
}

JNIEXPORT void JNICALL
Java_com_nerio_audioengine_EqProcessor_nativeDestroy(JNIEnv*, jclass, jlong handle) {
    auto* proc = reinterpret_cast<EqProcessor*>(handle);
    delete proc;
}

JNIEXPORT void JNICALL
Java_com_nerio_audioengine_EqProcessor_nativeConfigure(JNIEnv* env, jclass,
        jlong handle, jint numFilters, jdoubleArray coefficients,
        jdouble preamp, jint channelCount, jint encoding) {
    auto* proc = reinterpret_cast<EqProcessor*>(handle);
    if (!proc) return;

    jdouble* coeffs = env->GetDoubleArrayElements(coefficients, nullptr);
    if (!coeffs) return;

    proc->configure(numFilters, coeffs, preamp, channelCount, encoding);
    env->ReleaseDoubleArrayElements(coefficients, coeffs, JNI_ABORT);

    LOGI("configured: %d filters, preamp=%.2f, ch=%d, enc=%d",
         numFilters, (float)preamp, channelCount, encoding);
}

JNIEXPORT void JNICALL
Java_com_nerio_audioengine_EqProcessor_nativeProcess(JNIEnv* env, jclass,
        jlong handle, jbyteArray data, jint offset, jint length) {
    auto* proc = reinterpret_cast<EqProcessor*>(handle);
    if (!proc) return;

    jbyte* bytes = (jbyte*)env->GetPrimitiveArrayCritical(data, nullptr);
    if (!bytes) return;

    proc->process(reinterpret_cast<uint8_t*>(bytes) + offset, length);
    env->ReleasePrimitiveArrayCritical(data, bytes, 0);
}

JNIEXPORT void JNICALL
Java_com_nerio_audioengine_EqProcessor_nativeReset(JNIEnv*, jclass, jlong handle) {
    auto* proc = reinterpret_cast<EqProcessor*>(handle);
    if (proc) proc->reset();
}

JNIEXPORT void JNICALL
Java_com_nerio_audioengine_EqProcessor_nativeSetEnabled(JNIEnv*, jclass, jlong handle,
        jboolean enabled) {
    auto* proc = reinterpret_cast<EqProcessor*>(handle);
    if (proc) proc->setEnabled(enabled);
}

} // extern "C"
