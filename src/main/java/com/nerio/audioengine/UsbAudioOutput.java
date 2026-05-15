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
    private DsdMode dsdActiveMode = DsdMode.AUTO;
    private VolumeMode volumeMode = VolumeMode.AUTO;
    private float currentVolumeLinear = 0f;  // 0.0 = silence; reset on every configure()

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

        // Re-assert the current volume on the native side after a reconfigure.
        // configure() is called for every new track, so we MUST NOT reset to
        // silence here -- the reset belongs at the DAC-connect layer (MusicService).
        applyCurrentVolume();

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

        // Route every PCM path through a gain-applying native. Raw nativeWrite
        // is reserved for DSD (1-bit can't be attenuated; gain would be noise).
        if (dsdMode) {
            return UsbAudioNative.nativeWrite(nativeHandle, data, offset, length);
        }

        switch (inputEncoding) {
            case AudioFormat.ENCODING_PCM_FLOAT:
                return UsbAudioNative.nativeWriteFloat32(nativeHandle, data, offset, length);
            case AudioFormat.ENCODING_PCM_16BIT:
                return UsbAudioNative.nativeWriteInt16(nativeHandle, data, offset, length);
            case AudioFormat.ENCODING_PCM_24BIT_PACKED:
                return UsbAudioNative.nativeWriteInt24Packed(nativeHandle, data, offset, length);
            case AudioFormat.ENCODING_PCM_32BIT:
                return UsbAudioNative.nativeWriteInt32(nativeHandle, data, offset, length);
            default:
                // Unknown encoding -- fall back to raw passthrough.
                return UsbAudioNative.nativeWrite(nativeHandle, data, offset, length);
        }
    }

    public int getConfiguredBitDepth() {
        return dacBitDepth;
    }

    public boolean isDsdMode() {
        return dsdMode;
    }

    public DsdMode getDsdActiveMode() {
        return dsdActiveMode;
    }

    /**
     * Configure the USB output for DSD playback in the requested mode.
     *  - DOP   : 24-bit PCM at dsdRate/16   (DSD over PCM, universal)
     *  - NATIVE: 32-bit PCM at dsdRate/32   (raw DSD in PCM container)
     *  - AUTO  : try NATIVE, fall back to DOP if the DAC doesn't accept it
     *
     * Returns the mode that was actually configured, or null on total failure.
     * Caller is expected to handle PCM-conversion fallback when this returns null.
     */
    public DsdMode configureDsd(int dsdRate, int channelCount, DsdMode requested) {
        if (nativeHandle == 0) {
            if (!open()) {
                Log.e(TAG, "configureDsd: open failed");
                return null;
            }
        }
        if (started) {
            stop();
        }

        // AUTO prefers DOP because it works on any DSD-capable USB DAC without
        // DAC-specific alt-setting selection. NATIVE is reserved for explicit picks.
        DsdMode tryFirst = (requested == DsdMode.AUTO) ? DsdMode.DOP : requested;
        if (tryConfigureDsd(dsdRate, channelCount, tryFirst)) {
            dsdActiveMode = tryFirst;
            return tryFirst;
        }
        if (requested == DsdMode.AUTO && tryFirst == DsdMode.DOP) {
            Log.w(TAG, "configureDsd: DOP rejected, falling back to NATIVE");
            if (tryConfigureDsd(dsdRate, channelCount, DsdMode.NATIVE)) {
                dsdActiveMode = DsdMode.NATIVE;
                return DsdMode.NATIVE;
            }
        }
        Log.e(TAG, "configureDsd: no DSD mode accepted by DAC");
        return null;
    }

    public int getConfiguredSubslotSize() {
        if (nativeHandle == 0) return 0;
        return UsbAudioNative.nativeGetConfiguredSubslotSize(nativeHandle);
    }

    private boolean tryConfigureDsd(int dsdRate, int channelCount, DsdMode mode) {
        int rate, bits, encoding;
        boolean preferDsdAlt;
        switch (mode) {
            case NATIVE:
                rate = dsdRate / 32;
                bits = 32;
                encoding = AudioFormat.ENCODING_PCM_32BIT;
                preferDsdAlt = true;
                break;
            case DOP:
                rate = dsdRate / 16;
                bits = 24;
                encoding = AudioFormat.ENCODING_PCM_24BIT_PACKED;
                preferDsdAlt = false;
                break;
            default:
                return false;
        }

        boolean ok = preferDsdAlt
                ? UsbAudioNative.nativeConfigureForDsd(nativeHandle, rate, channelCount, bits, true)
                : UsbAudioNative.nativeConfigure(nativeHandle, rate, channelCount, bits);
        if (!ok) {
            Log.w(TAG, "tryConfigureDsd " + mode + ": nativeConfigure(" + rate
                    + "Hz, " + channelCount + "ch, " + bits + "bit, preferDsd=" + preferDsdAlt
                    + ") failed");
            return false;
        }

        int actualBits = UsbAudioNative.nativeGetConfiguredBitDepth(nativeHandle);
        if (actualBits != bits) {
            Log.w(TAG, "tryConfigureDsd " + mode + ": requested " + bits
                    + "bit, DAC reported " + actualBits + "bit");
            return false;
        }

        this.inputEncoding = encoding;
        this.dacBitDepth = actualBits;
        this.dsdMode = true;
        String dsdLabel;
        switch (dsdRate) {
            case 2822400: dsdLabel = "DSD64"; break;
            case 5644800: dsdLabel = "DSD128"; break;
            case 11289600: dsdLabel = "DSD256"; break;
            default: dsdLabel = "DSD"; break;
        }
        int subslot = UsbAudioNative.nativeGetConfiguredSubslotSize(nativeHandle);
        Log.i(TAG, "Configured DSD " + mode + ": " + dsdLabel + " "
                + rate + "Hz/" + bits + "bit/" + channelCount + "ch subslot=" + subslot);
        return true;
    }

    public int getUacVersion() {
        if (nativeHandle != 0) {
            return UsbAudioNative.nativeGetUacVersion(nativeHandle);
        }
        return 0;
    }

    // === Volume control ===
    // Slider position is linear 0..1. Internally we map to dB via a cube-root
    // taper, so 0.5 on the slider ~ -12 dB rather than -30 dB (perceived-linear).
    // Floor of the curve is -60 dB; slider == 0 means hard mute.

    private static final double SOFTWARE_DB_FLOOR = -60.0;

    public void setVolume(float linear01) {
        if (Float.isNaN(linear01)) linear01 = 0f;
        if (linear01 < 0f) linear01 = 0f;
        if (linear01 > 1f) linear01 = 1f;
        currentVolumeLinear = linear01;
        Log.i(TAG, "setVolume linear=" + linear01
                + " effective=" + getEffectiveVolumeMode()
                + " requestedMode=" + volumeMode
                + " hasHW=" + hasHardwareVolume()
                + " dsdMode=" + dsdMode
                + " encoding=" + encodingName(inputEncoding));
        applyCurrentVolume();
    }

    public void setVolumeMode(VolumeMode mode) {
        volumeMode = (mode == null) ? VolumeMode.AUTO : mode;
        applyCurrentVolume();
    }

    public VolumeMode getVolumeMode() {
        return volumeMode;
    }

    public VolumeMode getEffectiveVolumeMode() {
        if (volumeMode != VolumeMode.AUTO) return volumeMode;
        return hasHardwareVolume() ? VolumeMode.HARDWARE : VolumeMode.SOFTWARE;
    }

    public boolean hasHardwareVolume() {
        return nativeHandle != 0 && UsbAudioNative.nativeHasHardwareVolume(nativeHandle);
    }

    public int getVolumeMinDbQ8() {
        return nativeHandle != 0 ? UsbAudioNative.nativeGetVolumeMinDbQ8(nativeHandle) : 0;
    }

    public int getVolumeMaxDbQ8() {
        return nativeHandle != 0 ? UsbAudioNative.nativeGetVolumeMaxDbQ8(nativeHandle) : 0;
    }

    public float getVolumeLinear() {
        return currentVolumeLinear;
    }

    /** Current attenuation in dB for the active mode, or {@code -inf} if muted. */
    public double getVolumeDb() {
        if (currentVolumeLinear <= 0f) return Double.NEGATIVE_INFINITY;
        VolumeMode eff = getEffectiveVolumeMode();
        if (eff == VolumeMode.HARDWARE && hasHardwareVolume()) {
            double dbMin = Math.max(getVolumeMinDbQ8() / 256.0, SOFTWARE_DB_FLOOR);
            double dbMax = getVolumeMaxDbQ8() / 256.0;
            return dbMin + (dbMax - dbMin) * Math.cbrt(currentVolumeLinear);
        }
        return SOFTWARE_DB_FLOOR * (1.0 - Math.cbrt(currentVolumeLinear));
    }

    /** Convert a dB attenuation back to a slider position 0..1. Inverse of getVolumeDb. */
    public float dbToLinear(double db) {
        if (Double.isInfinite(db) && db < 0) return 0f;
        VolumeMode eff = getEffectiveVolumeMode();
        double s;
        if (eff == VolumeMode.HARDWARE && hasHardwareVolume()) {
            double dbMin = Math.max(getVolumeMinDbQ8() / 256.0, SOFTWARE_DB_FLOOR);
            double dbMax = getVolumeMaxDbQ8() / 256.0;
            double range = dbMax - dbMin;
            if (range <= 0) return 1f;
            double cbrtS = (db - dbMin) / range;
            if (cbrtS < 0) cbrtS = 0;
            if (cbrtS > 1) cbrtS = 1;
            s = cbrtS * cbrtS * cbrtS;
        } else {
            double cbrtS = 1.0 - (db / SOFTWARE_DB_FLOOR);
            if (cbrtS < 0) cbrtS = 0;
            if (cbrtS > 1) cbrtS = 1;
            s = cbrtS * cbrtS * cbrtS;
        }
        return (float) s;
    }

    private void applyCurrentVolume() {
        if (nativeHandle == 0) {
            Log.w(TAG, "applyCurrentVolume: no native handle");
            return;
        }
        final VolumeMode eff = getEffectiveVolumeMode();

        if (eff == VolumeMode.EXTERNAL) {
            // DAC stays at unity; downstream amp / DAC knob controls volume.
            Log.i(TAG, "applyVolume EXTERNAL: setting DAC to unity, gain=1.0");
            if (hasHardwareVolume()) {
                UsbAudioNative.nativeSetHardwareVolumeDbQ8(nativeHandle, getVolumeMaxDbQ8());
                if (UsbAudioNative.nativeHasHardwareMute(nativeHandle)) {
                    UsbAudioNative.nativeSetHardwareMute(nativeHandle, false);
                }
            }
            UsbAudioNative.nativeSetSoftwareGain(nativeHandle, 1.0f);
            return;
        }

        if (eff == VolumeMode.HARDWARE && hasHardwareVolume()) {
            UsbAudioNative.nativeSetSoftwareGain(nativeHandle, 1.0f);
            if (currentVolumeLinear <= 0f) {
                Log.i(TAG, "applyVolume HARDWARE: mute");
                if (UsbAudioNative.nativeHasHardwareMute(nativeHandle)) {
                    UsbAudioNative.nativeSetHardwareMute(nativeHandle, true);
                } else {
                    UsbAudioNative.nativeSetHardwareVolumeDbQ8(nativeHandle, getVolumeMinDbQ8());
                }
            } else {
                if (UsbAudioNative.nativeHasHardwareMute(nativeHandle)) {
                    UsbAudioNative.nativeSetHardwareMute(nativeHandle, false);
                }
                double dbMin = Math.max(getVolumeMinDbQ8() / 256.0, SOFTWARE_DB_FLOOR);
                double dbMax = getVolumeMaxDbQ8() / 256.0;
                double db = dbMin + (dbMax - dbMin) * Math.cbrt(currentVolumeLinear);
                int q8 = (int) Math.round(db * 256.0);
                Log.i(TAG, "applyVolume HARDWARE: db=" + db + " q8=" + q8);
                UsbAudioNative.nativeSetHardwareVolumeDbQ8(nativeHandle, q8);
            }
            return;
        }

        // SOFTWARE path (or AUTO->SOFTWARE because no FU_VOLUME).
        float gain;
        if (currentVolumeLinear <= 0f) {
            gain = 0f;
        } else {
            double db = SOFTWARE_DB_FLOOR * (1.0 - Math.cbrt(currentVolumeLinear));
            gain = (float) Math.pow(10.0, db / 20.0);
        }
        Log.i(TAG, "applyVolume SOFTWARE: gain=" + gain
                + " (note: applies only to PCM, not DSD)");
        UsbAudioNative.nativeSetSoftwareGain(nativeHandle, gain);
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
