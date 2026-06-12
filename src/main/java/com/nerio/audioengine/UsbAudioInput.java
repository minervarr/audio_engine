package com.nerio.audioengine;

import android.util.Log;

import java.io.File;
import java.io.IOException;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Capture view of a USB Audio Class device. Records PCM from the device's ADC
 * (line-in / microphone / etc.) into a canonical WAV file. May run full-duplex
 * alongside an {@link UsbAudioOutput} that shares the same native driver.
 *
 * <p>Lifecycle: obtained from {@link UsbAudioDevice#getInput()}. Call
 * {@link #configure(int, int, int)} and {@link #start()} before
 * {@link #startRecording(File)}; call {@link #stopRecording()} then
 * {@link #stop()} to tear down. {@link #release()} hands back the shared
 * native handle to the owning device.
 */
public class UsbAudioInput implements AudioInput {

    private static final String TAG = "UsbAudioInput";
    private static final int POLL_BUFFER_BYTES = 16 * 1024;
    private static final int IDLE_POLL_SLEEP_MS = 2;

    private final long nativeHandle;
    private final UsbAudioDevice owner;
    private volatile boolean started;
    private volatile boolean released;

    private int rate;
    private int channels;
    private int bitDepth;
    private int subslot;

    private Thread recordThread;
    private volatile boolean recording;
    private WavFileWriter wavWriter;
    private volatile UsbAudioOutput monitorOut;
    private volatile int monitorOutChannels;
    private byte[] monitorExpandBuf;
    private final AtomicLong framesWritten = new AtomicLong(0);

    UsbAudioInput(long nativeHandle, UsbAudioDevice owner) {
        this.nativeHandle = nativeHandle;
        this.owner = owner;
    }

    /** Returns true if the connected device exposes any capture-direction alt-settings. */
    public boolean isAvailable() {
        return nativeHandle != 0 && UsbAudioNative.nativeHasCaptureFormats(nativeHandle);
    }

    /** Sample rates exposed by the device's capture-direction alt-settings. */
    public int[] getSupportedRates() {
        return nativeHandle != 0
                ? UsbAudioNative.nativeGetSupportedCaptureRates(nativeHandle)
                : new int[0];
    }

    /** Bit depths exposed by the device's capture-direction alt-settings. */
    public int[] getSupportedBitDepths() {
        return nativeHandle != 0
                ? UsbAudioNative.nativeGetSupportedCaptureBitDepths(nativeHandle)
                : new int[0];
    }

    /** Channel counts exposed by the device's capture-direction alt-settings. */
    public int[] getSupportedChannelCounts() {
        return nativeHandle != 0
                ? UsbAudioNative.nativeGetSupportedCaptureChannelCounts(nativeHandle)
                : new int[0];
    }

    public boolean configure(int sampleRate, int channelCount, int requestedBitDepth) {
        if (nativeHandle == 0) {
            Log.e(TAG, "configure: no native handle");
            return false;
        }
        boolean ok = UsbAudioNative.nativeConfigureCapture(
                nativeHandle, sampleRate, channelCount, requestedBitDepth);
        if (!ok) {
            Log.e(TAG, "nativeConfigureCapture(" + sampleRate + "Hz, "
                    + channelCount + "ch, " + requestedBitDepth + "bit) failed");
            return false;
        }
        this.rate = UsbAudioNative.nativeGetConfiguredCaptureRate(nativeHandle);
        this.channels = UsbAudioNative.nativeGetConfiguredCaptureChannels(nativeHandle);
        this.bitDepth = UsbAudioNative.nativeGetConfiguredCaptureBitDepth(nativeHandle);
        this.subslot = UsbAudioNative.nativeGetConfiguredCaptureSubslotSize(nativeHandle);
        Log.i(TAG, "Configured capture: " + rate + "Hz/" + bitDepth + "bit/"
                + channels + "ch subslot=" + subslot);
        return true;
    }

    public boolean start() {
        if (nativeHandle == 0) return false;
        started = UsbAudioNative.nativeStartCapture(nativeHandle);
        if (!started) Log.e(TAG, "nativeStartCapture failed");
        return started;
    }

    /** Read raw wire-format PCM directly. Returns -1 if not capturing, 0 if no data ready. */
    public int read(byte[] buf, int offset, int maxLen) {
        return UsbAudioNative.nativeReadCapture(nativeHandle, buf, offset, maxLen);
    }

    /**
     * Routes a copy of every captured PCM block written to the WAV into {@code out}
     * for live monitoring through a {@link UsbAudioOutput}. The output must already
     * be configured and started; this class does not own its lifecycle.
     *
     * <p>When the monitor output's channel count exceeds the input's, the record
     * loop performs an N→M upmix: each frame is written with the original input
     * channels followed by copies of channel 0 in the trailing slots. For the
     * common case of mono mic → stereo headphones, each mono sample lands in both
     * L and R of the output stream.
     *
     * <p>Pass {@code null} for {@code out} to disable monitoring.
     */
    public void setMonitorOutput(UsbAudioOutput out, int outChannels) {
        this.monitorOutChannels = Math.max(channels, outChannels);
        this.monitorOut = out;
    }

    /** Back-compat alias: assumes the monitor output uses the same channel count as input. */
    public void setMonitorOutput(UsbAudioOutput out) {
        setMonitorOutput(out, channels);
    }

    public void stop() {
        stopRecording();
        if (started) {
            UsbAudioNative.nativeStopCapture(nativeHandle);
            started = false;
        }
    }

    public void release() {
        if (released) return;
        released = true;
        stop();
        if (owner != null) owner.notifyViewReleased();
    }

    /**
     * Starts a background thread that pulls PCM out of the native capture ring
     * buffer and appends it to {@code wavOut} as a canonical RIFF/WAVE file.
     * Subslot padding is stripped so the WAV holds packed PCM at the configured
     * bit depth. The file's RIFF/data sizes are patched on {@link #stopRecording()}.
     */
    public boolean startRecording(File wavOut) throws IOException {
        if (!started) {
            Log.e(TAG, "startRecording: capture not started");
            return false;
        }
        if (recording) return false;
        if (wavOut == null) throw new IOException("wavOut == null");

        wavWriter = new WavFileWriter(wavOut, rate, channels, bitDepth, false);
        framesWritten.set(0);

        recording = true;
        recordThread = new Thread(this::recordLoop, "UsbAudioInput-WAV");
        recordThread.start();
        Log.i(TAG, "startRecording -> " + wavOut.getAbsolutePath());
        return true;
    }

    public void stopRecording() {
        recording = false;
        Thread t = recordThread;
        recordThread = null;
        if (t != null) {
            try {
                t.join(2000);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
        }
        WavFileWriter w = wavWriter;
        wavWriter = null;
        if (w != null) {
            try {
                long bytes = w.getPcmBytes();
                w.close();
                Log.i(TAG, "stopRecording: wrote " + bytes + " PCM bytes");
            } catch (IOException e) {
                Log.e(TAG, "wav close failed", e);
            }
        }
    }

    private void recordLoop() {
        final int outBytesPerSample = (bitDepth + 7) / 8;
        final boolean stripPadding = subslot != outBytesPerSample;
        final byte[] wireBuf = new byte[POLL_BUFFER_BYTES];
        final byte[] packedBuf = stripPadding ? new byte[POLL_BUFFER_BYTES] : null;

        // Keep reading until the native ring buffer is empty AND `recording` is false
        // (drain phase) — only then exit. nativeReadCapture only returns -1 once
        // nativeStopCapture has torn the ring buffer down, which doesn't happen until
        // input.stop() runs after this thread joins.
        while (true) {
            int got = UsbAudioNative.nativeReadCapture(
                    nativeHandle, wireBuf, 0, wireBuf.length);
            if (got < 0) {
                Log.w(TAG, "nativeReadCapture returned " + got + " -- ending record loop");
                break;
            }
            if (got == 0) {
                if (!recording) break;
                try {
                    Thread.sleep(IDLE_POLL_SLEEP_MS);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    break;
                }
                continue;
            }
            try {
                final byte[] outBuf;
                final int outLen;
                if (stripPadding) {
                    outLen = stripSubslotPadding(
                            wireBuf, got, packedBuf, subslot, outBytesPerSample);
                    outBuf = packedBuf;
                } else {
                    outLen = got;
                    outBuf = wireBuf;
                }
                wavWriter.write(outBuf, 0, outLen);
                int frameBytes = outBytesPerSample * channels;
                if (frameBytes > 0) {
                    framesWritten.addAndGet(outLen / frameBytes);
                }
                UsbAudioOutput mo = monitorOut;
                if (mo != null) {
                    final int outCh = monitorOutChannels;
                    byte[] monitorBuf = outBuf;
                    int monitorLen = outLen;
                    if (outCh > channels && channels > 0) {
                        int needed = outLen * outCh / channels;
                        if (monitorExpandBuf == null || monitorExpandBuf.length < needed) {
                            monitorExpandBuf = new byte[needed];
                        }
                        monitorLen = expandChannels(outBuf, outLen, monitorExpandBuf,
                                outBytesPerSample, channels, outCh);
                        monitorBuf = monitorExpandBuf;
                    }
                    try {
                        mo.write(monitorBuf, 0, monitorLen);
                    } catch (Throwable t) {
                        Log.w(TAG, "monitor write failed; dropping monitor", t);
                        monitorOut = null;
                    }
                }
            } catch (IOException e) {
                Log.e(TAG, "wav write failed", e);
                break;
            }
        }
    }

    /**
     * Expand each PCM frame from {@code srcCh} channels to {@code dstCh}
     * channels (dstCh ≥ srcCh). Writes the srcCh source samples as-is,
     * then fills each of the (dstCh − srcCh) remaining output channels with
     * a copy of channel 0. For the common mono→stereo case (srcCh=1,
     * dstCh=2), each mono sample is written twice consecutively so it
     * appears on both L and R.
     */
    private static int expandChannels(byte[] in, int inLen, byte[] out,
                                      int bytesPerSample, int srcCh, int dstCh) {
        int srcFrameBytes = srcCh * bytesPerSample;
        int o = 0;
        for (int i = 0; i + srcFrameBytes <= inLen; i += srcFrameBytes) {
            for (int b = 0; b < srcFrameBytes; b++) {
                out[o++] = in[i + b];
            }
            int extras = dstCh - srcCh;
            for (int e = 0; e < extras; e++) {
                for (int b = 0; b < bytesPerSample; b++) {
                    out[o++] = in[i + b];
                }
            }
        }
        return o;
    }

    /**
     * UAC2 leaves the LSB bytes of a wider-than-needed subslot as zero padding
     * (e.g. 24-bit-in-32-bit: wire bytes little-endian are {@code [pad][b0][b1][b2]};
     * packed 24-bit WAV wants {@code [b0][b1][b2]}). Strips the leading
     * {@code subslot - outBytes} bytes of each sample.
     */
    private static int stripSubslotPadding(byte[] in, int inLen, byte[] out,
                                           int subslot, int outBytes) {
        int padBytes = subslot - outBytes;
        int o = 0;
        for (int i = 0; i + subslot <= inLen; i += subslot) {
            int from = i + padBytes;
            for (int b = 0; b < outBytes; b++) {
                out[o++] = in[from + b];
            }
        }
        return o;
    }

    public int getConfiguredRate() { return rate; }
    public int getConfiguredChannels() { return channels; }
    public int getConfiguredBitDepth() { return bitDepth; }
    public int getConfiguredSubslotSize() { return subslot; }
    public boolean isStarted() { return started; }
    public boolean isRecording() { return recording; }
    public long getFramesWritten() { return framesWritten.get(); }
}
