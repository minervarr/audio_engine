package com.nerio.audioengine;

import android.util.Log;

import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;

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
public class UsbAudioInput {

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
    private RandomAccessFile wavFile;
    private long bytesWritten;

    UsbAudioInput(long nativeHandle, UsbAudioDevice owner) {
        this.nativeHandle = nativeHandle;
        this.owner = owner;
    }

    /** Returns true if the connected device exposes any capture-direction alt-settings. */
    public boolean isAvailable() {
        return nativeHandle != 0 && UsbAudioNative.nativeHasCaptureFormats(nativeHandle);
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

        wavFile = new RandomAccessFile(wavOut, "rw");
        wavFile.setLength(0);
        writeWavHeader(wavFile, rate, channels, bitDepth);
        bytesWritten = 0;

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
        RandomAccessFile f = wavFile;
        wavFile = null;
        if (f != null) {
            try {
                patchWavSizes(f, bytesWritten);
                f.close();
                Log.i(TAG, "stopRecording: wrote " + bytesWritten + " PCM bytes");
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

        while (recording) {
            int got = UsbAudioNative.nativeReadCapture(
                    nativeHandle, wireBuf, 0, wireBuf.length);
            if (got < 0) {
                Log.w(TAG, "nativeReadCapture returned " + got + " -- ending record loop");
                break;
            }
            if (got == 0) {
                try {
                    Thread.sleep(IDLE_POLL_SLEEP_MS);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    break;
                }
                continue;
            }
            try {
                if (stripPadding) {
                    int outLen = stripSubslotPadding(
                            wireBuf, got, packedBuf, subslot, outBytesPerSample);
                    wavFile.write(packedBuf, 0, outLen);
                    bytesWritten += outLen;
                } else {
                    wavFile.write(wireBuf, 0, got);
                    bytesWritten += got;
                }
            } catch (IOException e) {
                Log.e(TAG, "wav write failed", e);
                break;
            }
        }
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

    private static void writeWavHeader(RandomAccessFile f, int rate, int channels, int bitDepth)
            throws IOException {
        int bytesPerSample = (bitDepth + 7) / 8;
        int blockAlign = channels * bytesPerSample;
        int byteRate = rate * blockAlign;
        // Canonical 44-byte PCM RIFF/WAVE. Sizes at offsets 4 and 40 stay zero
        // until patchWavSizes() rewrites them on stopRecording().
        f.writeBytes("RIFF");
        writeIntLE(f, 0);                 // [4..7]   ChunkSize (patched)
        f.writeBytes("WAVE");
        f.writeBytes("fmt ");
        writeIntLE(f, 16);                // Subchunk1Size = 16 for PCM
        writeShortLE(f, (short) 1);       // AudioFormat = 1 (PCM)
        writeShortLE(f, (short) channels);
        writeIntLE(f, rate);
        writeIntLE(f, byteRate);
        writeShortLE(f, (short) blockAlign);
        writeShortLE(f, (short) bitDepth);
        f.writeBytes("data");
        writeIntLE(f, 0);                 // [40..43] Subchunk2Size (patched)
    }

    private static void patchWavSizes(RandomAccessFile f, long pcmBytes) throws IOException {
        long riffSize = 36 + pcmBytes;
        if (riffSize > 0x7FFFFFFFL) riffSize = 0x7FFFFFFFL;
        long dataSize = Math.min(pcmBytes, 0x7FFFFFFFL);
        f.seek(4);
        writeIntLE(f, (int) riffSize);
        f.seek(40);
        writeIntLE(f, (int) dataSize);
    }

    private static void writeIntLE(RandomAccessFile f, int v) throws IOException {
        f.write(v & 0xFF);
        f.write((v >>> 8) & 0xFF);
        f.write((v >>> 16) & 0xFF);
        f.write((v >>> 24) & 0xFF);
    }

    private static void writeShortLE(RandomAccessFile f, short v) throws IOException {
        f.write(v & 0xFF);
        f.write((v >>> 8) & 0xFF);
    }

    public int getConfiguredRate() { return rate; }
    public int getConfiguredChannels() { return channels; }
    public int getConfiguredBitDepth() { return bitDepth; }
    public int getConfiguredSubslotSize() { return subslot; }
    public boolean isStarted() { return started; }
    public boolean isRecording() { return recording; }
}
