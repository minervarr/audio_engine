package com.nerio.audioengine;

public final class DsdPackager {

    private DsdPackager() {}

    /**
     * DSD-over-PCM (DoP 1.1) packing.
     * Wraps 16 MSB-first DSD bits per channel into a 24-bit DoP word:
     *   [marker | dsd_hi | dsd_lo]   (24-bit value, MSB to LSB)
     * Marker pattern per spec: frame 0 -> 0x05, frame 1 -> 0xFA, alternating.
     * Caller tracks the counter across calls so the pattern is continuous.
     *
     * The wire layout depends on the DAC's USB subslot size:
     *   subslotBytes == 3 (24-bit packed): write [dsd_lo, dsd_hi, marker]
     *   subslotBytes == 4 (24-bit-in-32):  write [0x00, dsd_lo, dsd_hi, marker]
     *                                       (LSB padding per UAC2 spec)
     * The 0x05/0xFA marker stays in the MSB byte either way, which is where any
     * DoP-aware DAC sniffs for the pattern.
     *
     * Frame count produced = srcLen / 2.
     * Bytes written = frames * channels * subslotBytes.
     */
    public static int packDop(byte[] left, byte[] right, int srcLen,
                              int channels, byte[] out, int[] frameCounter,
                              int subslotBytes) {
        int frames = srcLen / 2;
        int pos = 0;
        int counter = frameCounter[0];
        boolean pad = (subslotBytes == 4);

        for (int f = 0; f < frames; f++) {
            int markerByte = ((counter & 1) == 0) ? 0x05 : 0xFA;
            counter++;

            int li = f * 2;
            byte lHi = left[li];
            byte lLo = left[li + 1];

            if (pad) out[pos++] = 0;
            out[pos++] = lLo;
            out[pos++] = lHi;
            out[pos++] = (byte) markerByte;

            if (channels >= 2) {
                byte rHi = right[li];
                byte rLo = right[li + 1];
                if (pad) out[pos++] = 0;
                out[pos++] = rLo;
                out[pos++] = rHi;
                out[pos++] = (byte) markerByte;
            }
        }

        frameCounter[0] = counter;
        return pos;
    }

    /** Back-compat overload assuming 3-byte subslot (24-bit packed). */
    public static int packDop(byte[] left, byte[] right, int srcLen,
                              int channels, byte[] out, int[] frameCounter) {
        return packDop(left, right, srcLen, channels, out, frameCounter, 3);
    }

    /**
     * Native DSD packing (32-bit container, big-endian byte order within the
     * container so DSD temporal order is preserved at byte offsets 0..3).
     * Each output frame carries 32 MSB-first DSD bits per channel.
     *
     * Frame count produced = srcLen / 4. Bytes written = frames * channels * 4.
     */
    public static int packNative(byte[] left, byte[] right, int srcLen,
                                 int channels, byte[] out) {
        int frames = srcLen / 4;
        int pos = 0;

        for (int f = 0; f < frames; f++) {
            int li = f * 4;
            out[pos++] = left[li];
            out[pos++] = left[li + 1];
            out[pos++] = left[li + 2];
            out[pos++] = left[li + 3];

            if (channels >= 2) {
                out[pos++] = right[li];
                out[pos++] = right[li + 1];
                out[pos++] = right[li + 2];
                out[pos++] = right[li + 3];
            }
        }

        return pos;
    }

    /**
     * Quick-and-dirty DSD-to-PCM16 decimation for the speaker fallback path.
     * 16x decimation by boxcar (sum of 16 ±1 samples per output frame).
     * Sample rate of the output = dsdRate / 16. Encoding = PCM_16BIT little-endian.
     *
     * Frame count produced = srcLen / 2. Bytes written = frames * channels * 2.
     */
    public static int decimateToPcm16(byte[] left, byte[] right, int srcLen,
                                      int channels, byte[] out) {
        int frames = srcLen / 2;
        int pos = 0;

        for (int f = 0; f < frames; f++) {
            int li = f * 2;
            int lSum = popcountSigned(left[li]) + popcountSigned(left[li + 1]);
            int lPcm = clampInt16(lSum * 1024);
            out[pos++] = (byte) (lPcm & 0xFF);
            out[pos++] = (byte) ((lPcm >> 8) & 0xFF);

            if (channels >= 2) {
                int rSum = popcountSigned(right[li]) + popcountSigned(right[li + 1]);
                int rPcm = clampInt16(rSum * 1024);
                out[pos++] = (byte) (rPcm & 0xFF);
                out[pos++] = (byte) ((rPcm >> 8) & 0xFF);
            }
        }

        return pos;
    }

    private static int popcountSigned(byte b) {
        return Integer.bitCount(b & 0xFF) * 2 - 8;
    }

    private static int clampInt16(int v) {
        if (v > 32767) return 32767;
        if (v < -32768) return -32768;
        return v;
    }
}
