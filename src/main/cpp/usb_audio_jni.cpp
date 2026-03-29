#include <jni.h>
#include "usb_audio.h"
#include <android/log.h>
#include <unordered_map>
#include <mutex>

#define LOG_TAG "UsbAudioJNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static std::mutex driverMutex;
static UsbAudioDriver* activeDriver = nullptr;

extern "C" {

JNIEXPORT jlong JNICALL
Java_com_nerio_audioengine_UsbAudioNative_nativeOpen(JNIEnv* env, jclass, jint fd) {
    std::lock_guard<std::mutex> lock(driverMutex);
    if (activeDriver) {
        activeDriver->close();
        delete activeDriver;
    }

    auto* driver = new UsbAudioDriver();
    if (!driver->open(fd)) {
        delete driver;
        return 0;
    }

    if (!driver->parseDescriptors()) {
        driver->close();
        delete driver;
        return 0;
    }

    activeDriver = driver;
    return reinterpret_cast<jlong>(driver);
}

JNIEXPORT jintArray JNICALL
Java_com_nerio_audioengine_UsbAudioNative_nativeGetSupportedRates(JNIEnv* env, jclass, jlong handle) {
    auto* driver = reinterpret_cast<UsbAudioDriver*>(handle);
    if (!driver) return nullptr;

    auto rates = driver->getSupportedRates();
    jintArray result = env->NewIntArray(rates.size());
    if (result && !rates.empty()) {
        env->SetIntArrayRegion(result, 0, rates.size(), rates.data());
    }
    return result;
}

JNIEXPORT jboolean JNICALL
Java_com_nerio_audioengine_UsbAudioNative_nativeConfigure(JNIEnv*, jclass,
        jlong handle, jint sampleRate, jint channels, jint bitDepth) {
    auto* driver = reinterpret_cast<UsbAudioDriver*>(handle);
    if (!driver) return JNI_FALSE;
    return driver->configure(sampleRate, channels, bitDepth) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_nerio_audioengine_UsbAudioNative_nativeStart(JNIEnv*, jclass, jlong handle) {
    auto* driver = reinterpret_cast<UsbAudioDriver*>(handle);
    if (!driver) return JNI_FALSE;
    return driver->start() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_com_nerio_audioengine_UsbAudioNative_nativeWrite(JNIEnv* env, jclass,
        jlong handle, jbyteArray data, jint offset, jint length) {
    auto* driver = reinterpret_cast<UsbAudioDriver*>(handle);
    if (!driver) return -1;

    jbyte* bytes = env->GetByteArrayElements(data, nullptr);
    if (!bytes) return -1;

    int written = driver->write(reinterpret_cast<const uint8_t*>(bytes + offset), length);
    env->ReleaseByteArrayElements(data, bytes, JNI_ABORT);
    return written;
}

JNIEXPORT jint JNICALL
Java_com_nerio_audioengine_UsbAudioNative_nativeWriteFloat32(JNIEnv* env, jclass,
        jlong handle, jbyteArray data, jint offset, jint length) {
    auto* driver = reinterpret_cast<UsbAudioDriver*>(handle);
    if (!driver) return -1;

    jbyte* bytes = env->GetByteArrayElements(data, nullptr);
    if (!bytes) return -1;

    int numSamples = length / 4; // 4 bytes per float32 sample
    const float* floats = reinterpret_cast<const float*>(bytes + offset);
    int consumed = driver->writeFloat32(floats, numSamples);

    env->ReleaseByteArrayElements(data, bytes, JNI_ABORT);

    // Return bytes consumed (not samples)
    return consumed * 4;
}

JNIEXPORT jint JNICALL
Java_com_nerio_audioengine_UsbAudioNative_nativeWriteInt16(JNIEnv* env, jclass,
        jlong handle, jbyteArray data, jint offset, jint length) {
    auto* driver = reinterpret_cast<UsbAudioDriver*>(handle);
    if (!driver) return -1;

    jbyte* bytes = env->GetByteArrayElements(data, nullptr);
    if (!bytes) return -1;

    int numSamples = length / 2; // 2 bytes per int16 sample
    const int16_t* samples = reinterpret_cast<const int16_t*>(bytes + offset);
    int consumed = driver->writeInt16(samples, numSamples);

    env->ReleaseByteArrayElements(data, bytes, JNI_ABORT);

    // Return bytes consumed (not samples)
    return consumed * 2;
}

JNIEXPORT jint JNICALL
Java_com_nerio_audioengine_UsbAudioNative_nativeGetConfiguredBitDepth(JNIEnv*, jclass, jlong handle) {
    auto* driver = reinterpret_cast<UsbAudioDriver*>(handle);
    return driver ? driver->getConfiguredBitDepth() : 0;
}

JNIEXPORT jint JNICALL
Java_com_nerio_audioengine_UsbAudioNative_nativeGetUacVersion(JNIEnv*, jclass, jlong handle) {
    auto* driver = reinterpret_cast<UsbAudioDriver*>(handle);
    return driver ? driver->getUacVersion() : 0;
}

JNIEXPORT jstring JNICALL
Java_com_nerio_audioengine_UsbAudioNative_nativeGetDeviceInfo(JNIEnv* env, jclass, jlong handle) {
    auto* driver = reinterpret_cast<UsbAudioDriver*>(handle);
    if (!driver) return env->NewStringUTF("No device");

    std::string info = driver->getDeviceInfo();
    return env->NewStringUTF(info.c_str());
}

JNIEXPORT void JNICALL
Java_com_nerio_audioengine_UsbAudioNative_nativeStop(JNIEnv*, jclass, jlong handle) {
    auto* driver = reinterpret_cast<UsbAudioDriver*>(handle);
    if (driver) driver->stop();
}

JNIEXPORT void JNICALL
Java_com_nerio_audioengine_UsbAudioNative_nativeFlush(JNIEnv*, jclass, jlong handle) {
    auto* driver = reinterpret_cast<UsbAudioDriver*>(handle);
    if (driver) driver->flush();
}

JNIEXPORT void JNICALL
Java_com_nerio_audioengine_UsbAudioNative_nativeClose(JNIEnv*, jclass, jlong handle) {
    std::lock_guard<std::mutex> lock(driverMutex);
    auto* driver = reinterpret_cast<UsbAudioDriver*>(handle);
    if (driver) {
        driver->close();
        delete driver;
        if (activeDriver == driver) {
            activeDriver = nullptr;
        }
    }
}

} // extern "C"
