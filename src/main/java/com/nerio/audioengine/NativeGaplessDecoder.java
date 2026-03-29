package com.nerio.audioengine;

public class NativeGaplessDecoder {

    static {
        System.loadLibrary("matrix_audio");
    }

    private long handle;

    public NativeGaplessDecoder(int delayFrames, int paddingFrames, int frameSize,
                                NativePcmBuffer buffer) {
        handle = nativeCreate(delayFrames, paddingFrames, frameSize, buffer.getHandle());
    }

    public void processFrame(byte[] data, int offset, int length) throws InterruptedException {
        if (handle == 0) return;
        boolean ok = nativeProcessFrame(handle, data, offset, length);
        if (Thread.interrupted()) throw new InterruptedException();
    }

    public void signalEnd() {
        if (handle != 0) nativeSignalEnd(handle);
    }

    public void resetAfterSeek() {
        if (handle != 0) nativeResetAfterSeek(handle);
    }

    public void destroy() {
        if (handle != 0) {
            nativeDestroy(handle);
            handle = 0;
        }
    }

    private static native long nativeCreate(int delay, int padding, int frameSize, long bufferHandle);
    private static native void nativeDestroy(long handle);
    private static native boolean nativeProcessFrame(long handle, byte[] data, int offset, int length);
    private static native void nativeSignalEnd(long handle);
    private static native void nativeResetAfterSeek(long handle);
}
