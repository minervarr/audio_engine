package com.nerio.audioengine;

import android.media.AudioFormat;
import android.util.Log;

public class UsbAudioOutput implements AudioOutput {

    private static final String TAG = "UsbAudioOutput";

    private long nativeHandle;
    private final int fd;
    private boolean started;

    private int inputEncoding;
    private int dacBitDepth;
    private boolean dsdMode;

    public UsbAudioOutput(int fd) {
        this.fd = fd;
    }

    public boolean open() {
        if (nativeHandle == 0) {
            Log.i(TAG, "Opening native USB driver fd=" + fd);
            nativeHandle = UsbAudioNative.nativeOpen(fd);
            if (nativeHandle == 0) {
                Log.e(TAG, "nativeOpen returned 0 -- open failed");
            } else {
                Log.i(TAG, "nativeOpen OK handle=" + nativeHandle);
            }
        }
        return nativeHandle != 0;
    }

    @Override
    public boolean configure(int sampleRate, int channelCount, int encoding, int sourceBitDepth) {
        if (nativeHandle == 0) {
            if (!open()) {
                Log.e(TAG, "Failed to open USB device");
                return false;
            }
        }

        // Stop-before-reconfigure: native configure() handles this too,
        // but stop Java-side state as well
        if (started) {
            stop();
        }

        this.inputEncoding = encoding;
        dsdMode = sourceBitDepth == 1;

        // Use source bit depth to configure DAC for source-native playback.
        // DSD uses 32-bit container regardless of source depth.
        // 24-bit FLAC decoded to float -> request 24-bit from DAC
        // 16-bit FLAC decoded to 16-bit -> request 16-bit from DAC
        // Fallback to encoding-based derivation if sourceBitDepth unknown.
        int requestedBitDepth;
        if (dsdMode) {
            requestedBitDepth = 32;
        } else if (sourceBitDepth > 0) {
            requestedBitDepth = sourceBitDepth;
        } else {
            switch (encoding) {
                case AudioFormat.ENCODING_PCM_FLOAT:
                case AudioFormat.ENCODING_PCM_32BIT:
                    requestedBitDepth = 32;
                    break;
                case AudioFormat.ENCODING_PCM_24BIT_PACKED:
                    requestedBitDepth = 24;
                    break;
                default:
                    requestedBitDepth = 16;
                    break;
            }
        }

        if (!UsbAudioNative.nativeConfigure(nativeHandle, sampleRate, channelCount, requestedBitDepth)) {
            Log.e(TAG, "Failed to configure USB audio");
            return false;
        }

        dacBitDepth = UsbAudioNative.nativeGetConfiguredBitDepth(nativeHandle);
        if (dacBitDepth <= 0) {
            dacBitDepth = 16;
        }

        if (dsdMode) {
            String dsdLabel;
            switch (sampleRate) {
                case 88200: dsdLabel = "DSD64"; break;
                case 176400: dsdLabel = "DSD128"; break;
                case 352800: dsdLabel = "DSD256"; break;
                default: dsdLabel = "DSD"; break;
            }
            Log.i(TAG, "Configured DSD: rate=" + sampleRate + "(" + dsdLabel + ") dac=" + dacBitDepth + "bit");
        } else {
            Log.i(TAG, "Configured: inputEncoding=" + encodingName(inputEncoding)
                    + " source=" + sourceBitDepth + "bit"
                    + " requested=" + requestedBitDepth + "bit"
                    + " dac=" + dacBitDepth + "bit");
        }
        return true;
    }

    @Override
    public boolean start() {
        if (nativeHandle != 0) {
            started = UsbAudioNative.nativeStart(nativeHandle);
            if (!started) {
                Log.e(TAG, "Failed to start USB audio streaming");
            }
        }
        return started;
    }

    @Override
    public int write(byte[] data, int offset, int length) {
        if (nativeHandle == 0 || !started) return -1;

        if (inputEncoding == AudioFormat.ENCODING_PCM_FLOAT) {
            // Float32 -> DAC bit depth conversion in native C++
            return UsbAudioNative.nativeWriteFloat32(nativeHandle, data, offset, length);
        }

        if (inputEncoding == AudioFormat.ENCODING_PCM_16BIT && dacBitDepth != 16) {
            // 16-bit PCM but DAC configured at higher depth -- native upscaling
            return UsbAudioNative.nativeWriteInt16(nativeHandle, data, offset, length);
        }

        // Raw passthrough (bit-perfect when DAC bit depth matches input)
        return UsbAudioNative.nativeWrite(nativeHandle, data, offset, length);
    }

    public int getConfiguredBitDepth() {
        return dacBitDepth;
    }

    public boolean isDsdMode() {
        return dsdMode;
    }

    public int getUacVersion() {
        if (nativeHandle != 0) {
            return UsbAudioNative.nativeGetUacVersion(nativeHandle);
        }
        return 0;
    }

    public String getDeviceInfo() {
        if (nativeHandle != 0) {
            return UsbAudioNative.nativeGetDeviceInfo(nativeHandle);
        }
        return null;
    }

    @Override
    public void pause() {
        // USB isochronous keeps running but we stop feeding data
    }

    @Override
    public void resume() {
        // Resume feeding data
    }

    @Override
    public void flush() {
        if (nativeHandle != 0 && started) {
            UsbAudioNative.nativeFlush(nativeHandle);
        }
    }

    @Override
    public void stop() {
        if (nativeHandle != 0 && started) {
            UsbAudioNative.nativeStop(nativeHandle);
            started = false;
        }
    }

    @Override
    public void release() {
        stop();
        if (nativeHandle != 0) {
            UsbAudioNative.nativeClose(nativeHandle);
            nativeHandle = 0;
        }
    }

    public long getNativeHandle() {
        return nativeHandle;
    }

    public int[] getSupportedRates() {
        if (nativeHandle != 0) {
            return UsbAudioNative.nativeGetSupportedRates(nativeHandle);
        }
        return new int[0];
    }

    private static String encodingName(int encoding) {
        switch (encoding) {
            case AudioFormat.ENCODING_PCM_FLOAT: return "float32";
            case AudioFormat.ENCODING_PCM_24BIT_PACKED: return "int24";
            case AudioFormat.ENCODING_PCM_32BIT: return "int32";
            default: return "int16";
        }
    }
}
