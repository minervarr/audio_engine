package com.nerio.audioengine;

public class UsbAudioNative {

    static {
        System.loadLibrary("matrix_audio");
    }

    public static native long nativeOpen(int fd);
    public static native int[] nativeGetSupportedRates(long handle);
    public static native boolean nativeConfigure(long handle, int sampleRate, int channels, int bitDepth);
    public static native boolean nativeConfigureForDsd(long handle, int sampleRate, int channels, int bitDepth, boolean preferDsd);
    public static native boolean nativeStart(long handle);
    public static native int nativeWrite(long handle, byte[] data, int offset, int length);
    public static native void nativeStop(long handle);
    public static native void nativeFlush(long handle);
    public static native int nativeGetConfiguredBitDepth(long handle);
    public static native int nativeGetConfiguredSubslotSize(long handle);
    public static native int nativeWriteFloat32(long handle, byte[] data, int offset, int length);
    public static native int nativeWriteInt16(long handle, byte[] data, int offset, int length);
    public static native int nativeWriteInt24Packed(long handle, byte[] data, int offset, int length);
    public static native int nativeWriteInt32(long handle, byte[] data, int offset, int length);
    public static native int nativeGetUacVersion(long handle);
    public static native String nativeGetDeviceInfo(long handle);
    public static native void nativeClose(long handle);

    public static native boolean nativeHasHardwareVolume(long handle);
    public static native boolean nativeHasHardwareMute(long handle);
    public static native int nativeGetVolumeMinDbQ8(long handle);
    public static native int nativeGetVolumeMaxDbQ8(long handle);
    public static native boolean nativeSetHardwareVolumeDbQ8(long handle, int valueDbQ8);
    public static native boolean nativeSetHardwareMute(long handle, boolean muted);
    public static native void nativeSetSoftwareGain(long handle, float gain);

    // Capture (ADC -> host)
    public static native boolean nativeConfigureCapture(long handle, int sampleRate, int channels, int bitDepth);
    public static native boolean nativeStartCapture(long handle);
    public static native int nativeReadCapture(long handle, byte[] buf, int offset, int maxLen);
    public static native void nativeStopCapture(long handle);
    public static native int nativeGetConfiguredCaptureRate(long handle);
    public static native int nativeGetConfiguredCaptureChannels(long handle);
    public static native int nativeGetConfiguredCaptureBitDepth(long handle);
    public static native int nativeGetConfiguredCaptureSubslotSize(long handle);
    public static native boolean nativeHasCaptureFormats(long handle);
    public static native int[] nativeGetSupportedCaptureRates(long handle);
    public static native int[] nativeGetSupportedCaptureBitDepths(long handle);
    public static native int[] nativeGetSupportedCaptureChannelCounts(long handle);
}
