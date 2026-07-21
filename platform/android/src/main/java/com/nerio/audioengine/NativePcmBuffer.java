package com.nerio.audioengine;

public class NativePcmBuffer {

    static {
        System.loadLibrary("matrix_audio");
    }

    private long handle;

    public NativePcmBuffer(int capacity) {
        handle = nativeCreate(capacity);
    }

    public void write(byte[] data, int offset, int length) throws InterruptedException {
        if (handle == 0) return;
        boolean ok = nativeWrite(handle, data, offset, length);
        if (Thread.interrupted()) throw new InterruptedException();
        // !ok means flush was called during write -- treat like interrupt for control flow
    }

    public int read(byte[] dest, int offset, int maxLength) throws InterruptedException {
        if (handle == 0) return -1;
        int result = nativeRead(handle, dest, offset, maxLength);
        if (Thread.interrupted()) throw new InterruptedException();
        return result;
    }

    public void flush() {
        if (handle != 0) nativeFlush(handle);
    }

    public void signalEnd() {
        if (handle != 0) nativeSignalEnd(handle);
    }

    public void reset() {
        if (handle != 0) nativeReset(handle);
    }

    public long getHandle() {
        return handle;
    }

    public void destroy() {
        if (handle != 0) {
            nativeDestroy(handle);
            handle = 0;
        }
    }

    private static native long nativeCreate(int capacity);
    private static native void nativeDestroy(long handle);
    private static native boolean nativeWrite(long handle, byte[] data, int offset, int length);
    private static native int nativeRead(long handle, byte[] dest, int offset, int maxLength);
    private static native void nativeFlush(long handle);
    private static native void nativeSignalEnd(long handle);
    private static native void nativeReset(long handle);
}
