package com.nerio.audioengine;

import android.util.Log;

/**
 * Single-fd USB audio device. Owns the native libusb driver and exposes a
 * playback view ({@link UsbAudioOutput}) and a capture view ({@link UsbAudioInput}).
 * Both views share one libusb context and one device handle — libusb on Android
 * can only wrap a given USB fd once, so a second driver instance would fail.
 *
 * <p>Each view's {@code release()} decrements an internal refcount; the
 * underlying native driver is closed only when both views have released.
 */
public final class UsbAudioDevice {

    private static final String TAG = "UsbAudioDevice";

    private long nativeHandle;
    private final UsbAudioOutput output;
    private final UsbAudioInput input;
    private int openRefs;

    public UsbAudioDevice(int fd) {
        long handle = UsbAudioNative.nativeOpen(fd);
        if (handle == 0) {
            Log.e(TAG, "nativeOpen failed for fd=" + fd);
        }
        this.nativeHandle = handle;
        this.output = new UsbAudioOutput(fd, handle, this);
        this.input = new UsbAudioInput(handle, this);
        this.openRefs = (handle != 0) ? 2 : 0;
    }

    public boolean isValid() { return nativeHandle != 0; }
    public UsbAudioOutput getOutput() { return output; }
    public UsbAudioInput getInput() { return input; }
    public long getNativeHandle() { return nativeHandle; }

    /**
     * Called by each view's {@code release()}. When the last view releases,
     * the native driver is closed.
     */
    synchronized void notifyViewReleased() {
        if (nativeHandle == 0) return;
        if (--openRefs <= 0) {
            Log.i(TAG, "Last view released; closing native handle");
            UsbAudioNative.nativeClose(nativeHandle);
            nativeHandle = 0;
            openRefs = 0;
        }
    }

    /**
     * Force-release both views and close the native handle. Use this when
     * callers may not have touched the input view at all — it skips refcount
     * accounting and tears the device down unconditionally.
     */
    public synchronized void close() {
        if (nativeHandle == 0) return;
        try {
            output.release();
        } catch (Throwable t) {
            Log.w(TAG, "output.release threw", t);
        }
        try {
            input.release();
        } catch (Throwable t) {
            Log.w(TAG, "input.release threw", t);
        }
        // release() drives notifyViewReleased; if either path skipped (already
        // released), force the close here as a safety net.
        if (nativeHandle != 0) {
            UsbAudioNative.nativeClose(nativeHandle);
            nativeHandle = 0;
            openRefs = 0;
        }
    }
}
