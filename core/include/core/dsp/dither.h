#ifndef AE_CORE_DSP_DITHER_H
#define AE_CORE_DSP_DITHER_H

// TPDF dither + the single, final quantize to the device's wire grid.
//
// This is the last stage of the Reference EQ resample path: everything
// upstream stays in 64-bit double so that rounding happens exactly once, here.
// See docs/ref_eq_pipeline.md in the consuming app.
//
// It lives in the engine rather than in the player's view code so that
// tests/dsp_null_test.cpp can assert its output against a frozen reference —
// a quantizer whose correctness is argued rather than checked is not worth
// much on a bit-perfect signal path.

#include <cstdint>

#include "core/dsp/round.h"
#include "core/dsp/wire_scale.h"

namespace ae {

// Clamp to [-1, 1] and snap to the int32 wire grid, saturating.
//
// roundHalfAway is llround's exact semantics inlined. llround itself compiles
// to a libm call (`jmp llround@PLT`) under every flag short of -ffast-math,
// and this runs once per sample.
inline int32_t snapToWire(double s, double scale) {
    if (s >  1.0) s =  1.0;
    if (s < -1.0) s = -1.0;
    long long q = roundHalfAway(s * scale);
    if (q >  (long long)kInt32Max) q =  (long long)kInt32Max;
    if (q < (long long)kInt32Min) q = (long long)kInt32Min;
    return static_cast<int32_t>(q);
}

// Wire scale for a target bit depth. Output is always int32 wire format, with
// the unused low bits zeroed for 16/24-bit targets.
inline double wireScale(int bits) {
    return (bits == 16) ? kInt16Max * (double)(1 << 16)
         : (bits == 24) ? kInt24Max * (double)(1 << 8)
         :                kInt32Max;
}

// TPDF dither generator + quantizer. Holds its own LCG state so the noise
// sequence continues across calls: a generator restarted per buffer repeats
// the same noise every buffer, which is audible as a correlated tone rather
// than a flat noise floor.
class TpdfQuantizer {
public:
    // Quantize `n` samples to `bits`, dithering only when it buys something.
    //
    // The two loops are deliberate. At 32 bits the dither amplitude is zero,
    // and the old single-loop form still ran two LCG steps, a subtract and two
    // multiplies per sample to build a value it then scaled by zero and added
    // to nothing. It measured as an identical cost at 16, 24 and 32 bits —
    // the tell that full-depth output was paying for dither it discards.
    void process(const double* __restrict in, int32_t* __restrict out, int n, int bits) {
        const double scale = wireScale(bits);

        if (bits >= 32) {
            // Full depth: the quantization error already sits below any DAC's
            // noise floor, so dither would only spend headroom.
            for (int i = 0; i < n; i++) out[i] = snapToWire(in[i], scale);
            return;
        }

        // Reducing depth (e.g. 32-bit processing -> 16-bit Bluetooth): TPDF
        // replaces quantization distortion with uncorrelated noise, which is
        // far preferable to the harmonic artefacts of straight truncation.
        const double ditherAmp = 1.0 / scale;
        for (int i = 0; i < n; i++) {
            // Triangular distribution as the difference of two uniforms.
            const double r = ditherAmp * ((double)(int32_t)(next() >> 1) -
                                          (double)(int32_t)(next() >> 1))
                           * (1.0 / 1073741824.0);
            out[i] = snapToWire(in[i] + r, scale);
        }
    }

private:
    inline uint32_t next() {
        state = state * 1664525u + 1013904223u;
        return state;
    }
    uint32_t state = 0x9E3779B9u;
};

// TPDF dither + FIRST-ORDER NOISE SHAPING. Same dither as TpdfQuantizer, plus
// error feedback that pushes quantization noise toward the top of the band
// instead of leaving it flat — a real, audible noise-floor improvement below
// full depth, where the dither/quantization step is largest relative to the
// signal (most audible at 16-bit output).
//
// Deliberately a SEPARATE class, not a modification of TpdfQuantizer:
// TpdfQuantizer's flat-dither output is pinned exactly by
// tests/dsp_null_test.cpp and must keep passing unchanged. This is new,
// additively tested behaviour, wired in by the app only where it chooses to.
//
// Per-CHANNEL feedback state is required, not one shared scalar: samples
// arrive interleaved (L,R,L,R,...), and folding channel A's quantization
// error into channel B's next sample would leak one channel's shaped noise
// into the other's spectrum instead of shaping each channel's own noise
// independently.
class NoiseShapedQuantizer {
public:
    static constexpr int MAX_CHANNELS = 8;

    // Quantize `n` interleaved samples (n/channels frames) to `bits`. Same
    // >=32-bit bypass as TpdfQuantizer: full depth needs neither dither nor
    // shaping. `channels` beyond MAX_CHANNELS still get dithered and
    // quantized correctly, just without shaping (no feedback slot to carry
    // their error in) — this app is stereo-only in practice, MAX_CHANNELS
    // just matches EqProcessor's own ceiling rather than inventing a new one.
    void process(const double* __restrict in, int32_t* __restrict out, int n, int bits,
                 int channels) {
        const double scale = wireScale(bits);

        if (bits >= 32 || channels <= 0) {
            for (int i = 0; i < n; i++) out[i] = snapToWire(in[i], scale);
            return;
        }

        const double ditherAmp = 1.0 / scale;
        const int frames = n / channels;
        for (int f = 0; f < frames; f++) {
            const int base = f * channels;
            for (int c = 0; c < channels; c++) {
                const int i = base + c;
                const double r = ditherAmp * ((double)(int32_t)(next() >> 1) -
                                              (double)(int32_t)(next() >> 1))
                               * (1.0 / 1073741824.0);

                const double fb     = (c < MAX_CHANNELS) ? feedback[c] : 0.0;
                const double shaped = in[i] + fb;
                const int32_t q     = snapToWire(shaped + r, scale);
                out[i] = q;

                if (c < MAX_CHANNELS) {
                    // Error carried forward, in the same normalized units as
                    // `in`: what this sample SHOULD have been (post-feedback)
                    // minus what it actually became once quantized.
                    feedback[c] = shaped - (double)q / scale;
                }
            }
        }
    }

private:
    inline uint32_t next() {
        state = state * 1664525u + 1013904223u;
        return state;
    }
    uint32_t state = 0x9E3779B9u;
    double feedback[MAX_CHANNELS] = {};
};

} // namespace ae

#endif // AE_CORE_DSP_DITHER_H
