package com.nerio.audioengine;

import android.media.AudioFormat;

public class SignalPathInfo {

    // Source
    public String sourceFormat;   // "FLAC", "MP3", "DSD64", "WAV", etc.
    public int sourceRate;
    public int sourceBitDepth;
    public int sourceChannels;
    public String sourceMime;     // "audio/flac", "audio/mpeg", etc.
    public String sourceType;     // "LOCAL" or "TIDAL"
    public String tidalQuality;          // "HI_RES_LOSSLESS", "LOSSLESS", etc.
    public String tidalRequestedQuality; // what was requested (may differ from actual)
    public String tidalCodec;            // "flac", "mqa", etc.
    public long tidalFileSize;           // bytes

    // Decode
    public String codecName;      // "c2.android.flac.decoder", etc.
    public int decodedEncoding;   // AudioFormat.ENCODING_PCM_*
    public boolean isDsd;
    public int dsdRate;           // 2822400, 5644800, etc.
    public int dsdPcmRate;        // PCM rate after DSD conversion
    public String dsdPlaybackMode; // "Native", "DoP", or null for PCM

    // Output
    public String outputDevice;   // "USB DAC (UAC2)", "Speaker", "Bluetooth [Name]"
    public int outputRate;
    public int outputBitDepth;
    public int outputChannels;
    public int uacVersion;
    public String usbDeviceInfo;
    public int[] usbSupportedRates;
    public String writePathLabel; // "passthrough", "float32>int24", "int16>int24"
    public boolean isBitPerfect;

    // EQ
    public boolean eqActive;
    public String eqProfileName;

    public int getSourceQualityColor() {
        if (sourceFormat == null) return 0xFF1B5E20; // dim green
        String fmt = sourceFormat.toUpperCase();
        if (fmt.startsWith("DSD") || fmt.equals("FLAC") || fmt.equals("ALAC")
                || fmt.equals("WAV") || fmt.equals("AIFF") || fmt.equals("APE")) {
            return 0xFF00C853; // green - lossless
        }
        return 0xFFFFB300; // amber - lossy
    }

    public int getDecodeQualityColor() {
        if (isDsd && "Native".equals(dsdPlaybackMode)) {
            return 0xFF00C853; // green - native DSD, no conversion
        }
        if (isDsd) {
            return 0xFFFFB300; // amber - DSD>PCM conversion
        }
        // Check if decode changed bit depth
        int decodedBits = encodingToBits(decodedEncoding);
        if (decodedBits > 0 && sourceBitDepth > 0 && decodedBits != sourceBitDepth) {
            return 0xFFFFB300; // amber - bit depth conversion
        }
        return 0xFF00C853; // green - no conversion
    }

    public int getOutputQualityColor() {
        if (outputDevice == null) return 0xFF1B5E20;
        if (outputDevice.startsWith("USB")) {
            return isBitPerfect ? 0xFF00C853 : 0xFFFFB300;
        }
        return 0xFF1B5E20; // dim green - speaker/bluetooth through mixer
    }

    public String getDecodedEncodingName() {
        return encodingName(decodedEncoding);
    }

    private static int encodingToBits(int encoding) {
        switch (encoding) {
            case AudioFormat.ENCODING_PCM_FLOAT:
            case AudioFormat.ENCODING_PCM_32BIT:
                return 32;
            case AudioFormat.ENCODING_PCM_24BIT_PACKED:
                return 24;
            case AudioFormat.ENCODING_PCM_16BIT:
                return 16;
            default:
                return 0;
        }
    }

    private static String encodingName(int encoding) {
        switch (encoding) {
            case AudioFormat.ENCODING_PCM_FLOAT: return "PCM_FLOAT";
            case AudioFormat.ENCODING_PCM_32BIT: return "PCM_32BIT";
            case AudioFormat.ENCODING_PCM_24BIT_PACKED: return "PCM_24BIT";
            case AudioFormat.ENCODING_PCM_16BIT: return "PCM_16BIT";
            default: return "PCM_16BIT";
        }
    }
}
