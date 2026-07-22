#include <jni.h>
#include "core/dsp/audio_convert.h"

// Persists across calls; only the single AudioTrackOutput write thread uses it.
static DitherLCG g_ditherRng;

extern "C" {

JNIEXPORT void JNICALL
Java_com_nerio_audioengine_AudioTrackOutput_nativeFloatToInt16(JNIEnv* env, jclass,
        jbyteArray srcArray, jint srcOffset, jbyteArray dstArray, jint sampleCount) {

    jbyte* src = (jbyte*)env->GetPrimitiveArrayCritical(srcArray, nullptr);
    if (!src) return;
    jbyte* dst = (jbyte*)env->GetPrimitiveArrayCritical(dstArray, nullptr);
    if (!dst) {
        env->ReleasePrimitiveArrayCritical(srcArray, src, JNI_ABORT);
        return;
    }

    const float* srcFloats = reinterpret_cast<const float*>(src + srcOffset);
    int16_t* dstInt16 = reinterpret_cast<int16_t*>(dst);

    floatToInt16Dither(srcFloats, dstInt16, sampleCount, g_ditherRng);

    env->ReleasePrimitiveArrayCritical(dstArray, dst, 0);
    env->ReleasePrimitiveArrayCritical(srcArray, src, JNI_ABORT);
}

} // extern "C"
