#ifndef AE_CORE_DSD_PACKAGER_H
#define AE_CORE_DSD_PACKAGER_H

#include <cstdint>

namespace ae {

// Packs raw DSD block pairs (per-channel, MSB-first) into the wire layout the
// USB DAC expects. Pure computation — no OS, no allocation. Ported verbatim from
// the Java DsdPackager. `left`/`right` hold one block per channel; `out` must be
// sized for the worst case (DoP at 4-byte subslot = 2x input per channel).
class DsdPackager {
public:
    // DSD-over-PCM (DoP 1.1): 16 DSD bits/channel -> a 24-bit word carrying an
    // alternating 0x05/0xFA marker in the MSB byte. `frameCounter` persists the
    // marker phase across calls. Wire layout depends on the DAC subslot:
    //   subslot 3: [dsd_lo, dsd_hi, marker]
    //   subslot 4: [0x00, dsd_lo, dsd_hi, marker]   (LSB padding per UAC2)
    // Frames produced = srcLen/2. Returns bytes written.
    static int packDop(const uint8_t* left, const uint8_t* right, int srcLen,
                       int channels, uint8_t* out, int& frameCounter,
                       int subslotBytes) {
        int frames = srcLen / 2;
        int pos = 0;
        int counter = frameCounter;
        bool pad = (subslotBytes == 4);

        for (int f = 0; f < frames; f++) {
            int markerByte = ((counter & 1) == 0) ? 0x05 : 0xFA;
            counter++;

            int li = f * 2;
            uint8_t lHi = left[li];
            uint8_t lLo = left[li + 1];

            if (pad) out[pos++] = 0;
            out[pos++] = lLo;
            out[pos++] = lHi;
            out[pos++] = (uint8_t) markerByte;

            if (channels >= 2) {
                uint8_t rHi = right[li];
                uint8_t rLo = right[li + 1];
                if (pad) out[pos++] = 0;
                out[pos++] = rLo;
                out[pos++] = rHi;
                out[pos++] = (uint8_t) markerByte;
            }
        }
        frameCounter = counter;
        return pos;
    }

    // Native DSD in a 32-bit container (big-endian byte order so DSD temporal
    // order is preserved). 32 DSD bits/channel per frame.
    // Frames produced = srcLen/4. Returns bytes written.
    static int packNative(const uint8_t* left, const uint8_t* right, int srcLen,
                          int channels, uint8_t* out) {
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

    // 16x boxcar decimation to 16-bit PCM (little-endian) for the speaker
    // fallback. Output rate = dsdRate/16. Frames produced = srcLen/2.
    static int decimateToPcm16(const uint8_t* left, const uint8_t* right, int srcLen,
                               int channels, uint8_t* out) {
        int frames = srcLen / 2;
        int pos = 0;
        for (int f = 0; f < frames; f++) {
            int li = f * 2;
            int lSum = popcountSigned(left[li]) + popcountSigned(left[li + 1]);
            int lPcm = clampInt16(lSum * 1024);
            out[pos++] = (uint8_t) (lPcm & 0xFF);
            out[pos++] = (uint8_t) ((lPcm >> 8) & 0xFF);
            if (channels >= 2) {
                int rSum = popcountSigned(right[li]) + popcountSigned(right[li + 1]);
                int rPcm = clampInt16(rSum * 1024);
                out[pos++] = (uint8_t) (rPcm & 0xFF);
                out[pos++] = (uint8_t) ((rPcm >> 8) & 0xFF);
            }
        }
        return pos;
    }

private:
    static int popcountSigned(uint8_t b) {
        return __builtin_popcount((unsigned)b) * 2 - 8;
    }
    static int clampInt16(int v) {
        if (v > 32767) return 32767;
        if (v < -32768) return -32768;
        return v;
    }
};

} // namespace ae

#endif // AE_CORE_DSD_PACKAGER_H
