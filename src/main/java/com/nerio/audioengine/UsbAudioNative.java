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
    public static native int nativeGetConfiguredRate(long handle);
    public static native int nativeGetConfiguredChannels(long handle);
    public static native int nativeWriteFloat32(long handle, byte[] data, int offset, int length);
    public static native int nativeWriteInt16(long handle, byte[] data, int offset, int length);
    public static native int nativeWriteInt24Packed(long handle, byte[] data, int offset, int length);
    public static native int nativeWriteInt32(long handle, byte[] data, int offset, int length);
    public static native int nativeGetUacVersion(long handle);
    public static native String nativeGetDeviceInfo(long handle);
    public static native void nativeClose(long handle);
    public static native void nativeSetPaused(long handle, boolean paused);

    public static native boolean nativeHasHardwareVolume(long handle);
    public static native boolean nativeHasHardwareMute(long handle);
    public static native int nativeGetVolumeMinDbQ8(long handle);
    public static native int nativeGetVolumeMaxDbQ8(long handle);
    public static native boolean nativeSetHardwareVolumeDbQ8(long handle, int valueDbQ8);
    public static native boolean nativeSetHardwareMute(long handle, boolean muted);
    public static native void nativeSetSoftwareGain(long handle, float gain);

    // True when the integer write paths pass samples through untouched
    // (software gain at unity skips the float multiply/requantize entirely).
    public static native boolean nativeIsBitPerfectPath(long handle);

    // Latency/stability profile: 0 = LOW_LATENCY, 1 = STABLE (default).
    // Sizes iso transfer queues and ring buffers for both directions.
    // Must be called before configure()/startCapture(); ignored mid-stream.
    public static native void nativeSetLatencyProfile(long handle, int profile);

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

    // Output (DAC) capability introspection. See companion C++ in usb_audio.cpp.
    public static native int[] nativeGetSupportedOutputRates(long handle);
    public static native int[] nativeGetSupportedOutputBitDepths(long handle);
    public static native int[] nativeGetSupportedOutputChannelCounts(long handle);

    // Full (rate, channels, bits) tuples flattened 3 ints per format. The
    // per-axis lists above can't express coupled limits like "384 kHz only
    // at 16-bit"; a best-format picker needs the real tuples.
    public static native int[] nativeGetCaptureFormats(long handle);
    public static native int[] nativeGetOutputFormats(long handle);
}
