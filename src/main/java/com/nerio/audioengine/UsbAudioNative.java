package com.nerio.audioengine;

public class UsbAudioNative {

    static {
        System.loadLibrary("matrix_audio");
    }

    public static native long nativeOpen(int fd);
    public static native int[] nativeGetSupportedRates(long handle);
    public static native boolean nativeConfigure(long handle, int sampleRate, int channels, int bitDepth);
    public static native boolean nativeStart(long handle);
    public static native int nativeWrite(long handle, byte[] data, int offset, int length);
    public static native void nativeStop(long handle);
    public static native void nativeFlush(long handle);
    public static native int nativeGetConfiguredBitDepth(long handle);
    public static native int nativeWriteFloat32(long handle, byte[] data, int offset, int length);
    public static native int nativeWriteInt16(long handle, byte[] data, int offset, int length);
    public static native int nativeGetUacVersion(long handle);
    public static native String nativeGetDeviceInfo(long handle);
    public static native void nativeClose(long handle);
}
