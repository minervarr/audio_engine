#ifndef EQ_PROCESSOR_H
#define EQ_PROCESSOR_H

#include <cstdint>
#include <cstring>
#include <cmath>

#include "core/dsp/round.h"
#include "core/dsp/wire_scale.h"

// ---------------------------------------------------------------------------
// Cascaded biquad EQ, 64-bit double throughout.
//
// Two structural properties matter, and both are load-bearing:
//
// 1. FRAME-OUTER, FILTER-INNER — a whole frame walks the cascade before the
//    next frame starts. This looks like the "obvious" naive ordering and it is
//    the right one, for a reason worth recording because the alternative is
//    tempting and measurably worse:
//
//    A biquad is a serial recurrence, so the only parallelism available is
//    across CHANNELS. Ordering the loops filter-outer/block-inner (apply one
//    filter to 256 frames, then the next) exposes the recurrence itself as the
//    critical path: z1 -> y -> z1 is ~16 cycles, paid once per frame per
//    filter, with nothing to overlap it. Frame-outer instead puts the CASCADE
//    on the critical path — 10 filters x ~8 cycles of mul+add — and the z1/z2
//    updates then have a full cascade of slack before the next frame needs
//    them. Measured on an i7-11800H with a 10-band profile: block-inner
//    18.7 ns/sample, frame-outer 8.8. The block form was tried and reverted.
//
// 2. NO FUSED MULTIPLY-ADD, ANYWHERE. The SIMD paths below deliberately use
//    separate multiply and add instructions. FMA would be marginally more
//    accurate (one fewer rounding per tap), but it would make the ARM and x86
//    builds produce different bits from each other and from the scalar
//    reference. For an engine whose whole premise is identical bit-perfect
//    playback on every platform, reproducibility wins over a fraction of an
//    LSB. The previous NEON path used vfmaq_f64 and did diverge; it no
//    longer does.
//
// tests/dsp_null_test.cpp holds a frozen copy of the original scalar
// implementation and asserts exact equality against everything here.
// ---------------------------------------------------------------------------

#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#define EQ_HAS_NEON 1
#else
#define EQ_HAS_NEON 0
#endif

#if defined(__x86_64__) || defined(_M_X64) || defined(__SSE2__) || defined(_M_IX86_FP)
#include <emmintrin.h>
#define EQ_HAS_SSE2 1
#else
#define EQ_HAS_SSE2 0
#endif

class EqProcessor {
public:
    static constexpr int MAX_FILTERS  = 10;
    // Delay-line width. configure() clamps to this: the per-filter z1/z2 arrays
    // are fixed-size, and the old code took the caller's channel count on trust.
    static constexpr int MAX_CHANNELS = 8;
    // Samples processed per pass. Sized so the working set (256 frames x 8 ch x
    // 8 bytes = 16 KB) stays in L1/L2 while still amortising the per-filter
    // setup over enough samples to matter.
    static constexpr int BLOCK_FRAMES = 256;

    EqProcessor() : numFilters(0), preampLinear(1.0f), hasPreamp(false),
                    channelCount(2), encoding(0), enabled(true) {
        reset();
    }

    ~EqProcessor() = default;

    // encoding constants matching Android AudioFormat:
    // 2 = PCM_16BIT, 3 = PCM_8BIT, 4 = PCM_FLOAT, 21 = PCM_24BIT_PACKED, 22 = PCM_32BIT
    void configure(int nFilters, const double coeffs[], double preamp, int channels, int enc) {
        numFilters = nFilters > MAX_FILTERS ? MAX_FILTERS : nFilters;
        preampLinear = static_cast<float>(preamp);
        hasPreamp = std::abs(preamp - 1.0) > 1e-9;
        channelCount = channels > 0 ? channels : 2;
        if (channelCount > MAX_CHANNELS) channelCount = MAX_CHANNELS;
        encoding = enc;

        // coeffs layout: [b0, b1, b2, a1, a2] per filter (a0 normalized to 1)
        for (int i = 0; i < numFilters; i++) {
            int off = i * 5;
            filters[i].b0 = coeffs[off + 0];
            filters[i].b1 = coeffs[off + 1];
            filters[i].b2 = coeffs[off + 2];
            filters[i].a1 = coeffs[off + 3];
            filters[i].a2 = coeffs[off + 4];
        }

        reset();
    }

    void process(uint8_t* data, int length) {
        if (!enabled || numFilters == 0) return;

        switch (encoding) {
            case 4:  // PCM_FLOAT (32-bit float)
                processFloat(reinterpret_cast<float*>(data), length / 4);
                break;
            case 2:  // PCM_16BIT
                process16(reinterpret_cast<int16_t*>(data), length / 2);
                break;
            case 21: // PCM_24BIT_PACKED
                process24(data, length);
                break;
            case 22: // PCM_32BIT
                process32(reinterpret_cast<int32_t*>(data), length / 4);
                break;
            default:
                break;
        }
    }

    void reset() {
        for (int f = 0; f < MAX_FILTERS; f++) {
            for (int ch = 0; ch < MAX_CHANNELS; ch++) {
                filters[f].z1[ch] = 0.0;
                filters[f].z2[ch] = 0.0;
            }
        }
    }

    void setEnabled(bool en) {
        if (!en && enabled) {
            // Transitioning to disabled -- clear state so re-enable is clean
            reset();
        }
        enabled = en;
    }

    bool isEnabled() const { return enabled; }

    // EQ int32 input and write to double[] without quantizing back.
    // Used when a downstream stage (resample + dither) handles the final quantize.
    void processToDouble(const int32_t* __restrict in, double* __restrict out, int count) {
        // Scale straight into the caller's buffer, then filter it in place —
        // no scratch needed, and the conversion loop vectorises.
        //
        // Deliberately does NOT consult `enabled`, matching the original: this
        // entry point is only reached via EqManager when a profile is active,
        // and processBlock with numFilters == 0 still applies preamp, which is
        // what the old per-sample path did.
        scaleToDouble(in, out, count);
        processBlock(out, count / channelCount);
    }

    // Bypass: just scale int32 to double without filter math.
    static void scaleToDouble(const int32_t* __restrict in, double* __restrict out, int count) {
        for (int i = 0; i < count; i++)
            out[i] = in[i] * (1.0 / ae::kInt32Max);
    }

private:
    struct Biquad {
        double b0, b1, b2, a1, a2;
        double z1[MAX_CHANNELS]; // per-channel delay line (transposed direct form II)
        double z2[MAX_CHANNELS];
    };

    Biquad filters[MAX_FILTERS];
    int numFilters;
    float preampLinear;
    bool hasPreamp;
    int channelCount;
    int encoding;
    bool enabled;

    // Staging for the integer/float entry points. A fixed member array, so the
    // audio thread never allocates.
    //
    // Costs ~0.15 ns/sample versus converting inline, because the data is
    // walked three times (convert in, filter, convert out) instead of once.
    // That is a real regression for a 1-filter profile and an irrelevant one
    // for the 5-10 band profiles people actually use, where the cascade
    // dominates and the whole path is ~1.7x faster than before. Fusing it away
    // would mean duplicating the cascade into all four format entry points
    // across three SIMD backends; not worth that for 0.15 ns.
    double scratch[BLOCK_FRAMES * MAX_CHANNELS];

    // -----------------------------------------------------------------------
    // The cascade. `io` is interleaved, `frames` frames of channelCount each.
    // -----------------------------------------------------------------------
    // Preamp is a single scale applied to each sample before the cascade.
    // preampLinear is a float member and the original wrote
    // `sample * preampLinear`, which promotes to double — so the multiplier is
    // (double)preampLinear, not some higher-precision value. Keep it that way.
    // Folded into the cascade loops rather than run as its own pass over the
    // buffer: `hasPreamp` is loop-invariant, so the compiler unswitches it out.
    void processBlock(double* __restrict io, int frames) {
        if (frames <= 0) return;
        if (channelCount == 2) processStereoCascade(io, frames);
        else                   processGenericCascade(io, frames);
    }

    // Stereo. L and R sit adjacent in the interleaved buffer, and z1[0]/z1[1]
    // are adjacent too, so a 128-bit double vector maps onto the pair with no
    // shuffling — which is what makes the stereo case worth special-casing.
    // One frame walks the full cascade before the next frame begins; see the
    // loop-ordering note at the top of this file.
    void processStereoCascade(double* __restrict io, int frames) {
#if EQ_HAS_SSE2
        const __m128d preamp = _mm_set1_pd(static_cast<double>(preampLinear));
        for (int i = 0; i < frames; i++) {
            __m128d x = _mm_loadu_pd(io + i * 2);
            if (hasPreamp) x = _mm_mul_pd(x, preamp);
            for (int f = 0; f < numFilters; f++) {
                Biquad& bq = filters[f];
                const __m128d z1 = _mm_loadu_pd(&bq.z1[0]);
                const __m128d z2 = _mm_loadu_pd(&bq.z2[0]);
                // y  = b0*x + z1
                const __m128d y = _mm_add_pd(_mm_mul_pd(_mm_set1_pd(bq.b0), x), z1);
                // z1 = b1*x - a1*y + z2   (grouped exactly as the scalar form)
                _mm_storeu_pd(&bq.z1[0],
                    _mm_add_pd(_mm_sub_pd(_mm_mul_pd(_mm_set1_pd(bq.b1), x),
                                          _mm_mul_pd(_mm_set1_pd(bq.a1), y)), z2));
                // z2 = b2*x - a2*y
                _mm_storeu_pd(&bq.z2[0],
                    _mm_sub_pd(_mm_mul_pd(_mm_set1_pd(bq.b2), x),
                               _mm_mul_pd(_mm_set1_pd(bq.a2), y)));
                x = y;
            }
            _mm_storeu_pd(io + i * 2, x);
        }
#elif EQ_HAS_NEON
        const float64x2_t preamp = vdupq_n_f64(static_cast<double>(preampLinear));
        for (int i = 0; i < frames; i++) {
            float64x2_t x = vld1q_f64(io + i * 2);
            if (hasPreamp) x = vmulq_f64(x, preamp);
            for (int f = 0; f < numFilters; f++) {
                Biquad& bq = filters[f];
                const float64x2_t z1 = vld1q_f64(&bq.z1[0]);
                const float64x2_t z2 = vld1q_f64(&bq.z2[0]);
                // Separate mul/add/sub, NOT vfmaq_f64 — see the FMA note up top.
                const float64x2_t y = vaddq_f64(vmulq_f64(vdupq_n_f64(bq.b0), x), z1);
                vst1q_f64(&bq.z1[0],
                    vaddq_f64(vsubq_f64(vmulq_f64(vdupq_n_f64(bq.b1), x),
                                        vmulq_f64(vdupq_n_f64(bq.a1), y)), z2));
                vst1q_f64(&bq.z2[0],
                    vsubq_f64(vmulq_f64(vdupq_n_f64(bq.b2), x),
                              vmulq_f64(vdupq_n_f64(bq.a2), y)));
                x = y;
            }
            vst1q_f64(io + i * 2, x);
        }
#else
        // Portable fallback: L and R kept as two explicitly interleaved scalar
        // chains, so the compiler has two independent recurrences to overlap
        // even without vectors.
        const double preamp = static_cast<double>(preampLinear);
        for (int i = 0; i < frames; i++) {
            double xL = io[i * 2], xR = io[i * 2 + 1];
            if (hasPreamp) { xL *= preamp; xR *= preamp; }
            for (int f = 0; f < numFilters; f++) {
                Biquad& bq = filters[f];
                const double yL = bq.b0 * xL + bq.z1[0];
                const double yR = bq.b0 * xR + bq.z1[1];
                bq.z1[0] = bq.b1 * xL - bq.a1 * yL + bq.z2[0];
                bq.z1[1] = bq.b1 * xR - bq.a1 * yR + bq.z2[1];
                bq.z2[0] = bq.b2 * xL - bq.a2 * yL;
                bq.z2[1] = bq.b2 * xR - bq.a2 * yR;
                xL = yL; xR = yR;
            }
            io[i * 2]     = xL;
            io[i * 2 + 1] = xR;
        }
#endif
    }

    // Mono, or anything above stereo. Channel-innermost gives the CPU one
    // independent recurrence per channel to overlap, for the same reason the
    // stereo path pairs L and R.
    void processGenericCascade(double* __restrict io, int frames) {
        const int ch = channelCount;
        const double preamp = static_cast<double>(preampLinear);
        for (int i = 0; i < frames; i++) {
            double* p = io + (size_t)i * ch;
            if (hasPreamp) for (int c = 0; c < ch; c++) p[c] *= preamp;
            for (int f = 0; f < numFilters; f++) {
                Biquad& bq = filters[f];
                for (int c = 0; c < ch; c++) {
                    const double x = p[c];
                    const double y = bq.b0 * x + bq.z1[c];
                    bq.z1[c] = bq.b1 * x - bq.a1 * y + bq.z2[c];
                    bq.z2[c] = bq.b2 * x - bq.a2 * y;
                    p[c] = y;
                }
            }
        }
    }

    // Symmetric quantize of a double in [-1,1] to the full int32 grid with
    // rounding (not truncation). Used by the bit-transparent Reference EQ path:
    // the EQ math runs in double, this is the single, final snap to the wire.
    // ae::roundHalfAway is llround's exact semantics without llround's libm
    // call — see core/dsp/round.h.
    static inline int32_t quantize32(double s) {
        if (s > 1.0) s = 1.0; else if (s < -1.0) s = -1.0;
        long long q = ae::roundHalfAway(s * ae::kInt32Max);
        if (q > (long long)ae::kInt32Max) q = (long long)ae::kInt32Max;
        else if (q < (long long)ae::kInt32Min) q = (long long)ae::kInt32Min;
        return static_cast<int32_t>(q);
    }

    // Same rounding discipline as quantize32, scaled to 16/24-bit grids.
    // process16/process24 used to truncate toward zero here (a bare
    // static_cast), which is a correlated-distortion bug at these depths for
    // exactly the reason quantize32's own comment explains — round-to-nearest
    // is what "single, final snap to the wire" is supposed to mean regardless
    // of target width.
    static inline int16_t quantize16(double s) {
        if (s > 1.0) s = 1.0; else if (s < -1.0) s = -1.0;
        long long q = ae::roundHalfAway(s * ae::kInt16Max);
        if (q > (long long)ae::kInt16Max) q = (long long)ae::kInt16Max;
        else if (q < (long long)ae::kInt16Min) q = (long long)ae::kInt16Min;
        return static_cast<int16_t>(q);
    }

    static inline int32_t quantize24(double s) {
        if (s > 1.0) s = 1.0; else if (s < -1.0) s = -1.0;
        long long q = ae::roundHalfAway(s * ae::kInt24Max);
        if (q > (long long)ae::kInt24Max) q = (long long)ae::kInt24Max;
        else if (q < (long long)ae::kInt24Min) q = (long long)ae::kInt24Min;
        return static_cast<int32_t>(q);
    }

    // -----------------------------------------------------------------------
    // Format entry points. Each converts a block into `scratch`, runs the
    // cascade, and converts back — chunked at BLOCK_FRAMES so the staging
    // buffer is a fixed member rather than a length-dependent allocation.
    // -----------------------------------------------------------------------

    void processFloat(float* samples, int count) {
        const int ch = channelCount;
        const int totalFrames = count / ch;
        for (int base = 0; base < totalFrames; base += BLOCK_FRAMES) {
            const int frames = (totalFrames - base) < BLOCK_FRAMES
                             ? (totalFrames - base) : BLOCK_FRAMES;
            const int n = frames * ch;
            float* p = samples + (size_t)base * ch;
            for (int i = 0; i < n; i++) scratch[i] = p[i];
            processBlock(scratch, frames);
            for (int i = 0; i < n; i++) p[i] = static_cast<float>(scratch[i]);
        }
    }

    void process16(int16_t* samples, int count) {
        const int ch = channelCount;
        const int totalFrames = count / ch;
        for (int base = 0; base < totalFrames; base += BLOCK_FRAMES) {
            const int frames = (totalFrames - base) < BLOCK_FRAMES
                             ? (totalFrames - base) : BLOCK_FRAMES;
            const int n = frames * ch;
            int16_t* p = samples + (size_t)base * ch;
            for (int i = 0; i < n; i++) scratch[i] = p[i] / 32768.0;
            processBlock(scratch, frames);
            for (int i = 0; i < n; i++) p[i] = quantize16(scratch[i]);
        }
    }

    void process24(uint8_t* data, int length) {
        const int ch = channelCount;
        const int totalFrames = (length / 3) / ch;
        for (int base = 0; base < totalFrames; base += BLOCK_FRAMES) {
            const int frames = (totalFrames - base) < BLOCK_FRAMES
                             ? (totalFrames - base) : BLOCK_FRAMES;
            const int n = frames * ch;
            uint8_t* p = data + (size_t)base * ch * 3;
            for (int i = 0; i < n; i++) {
                const int off = i * 3;
                int32_t val = p[off] | (p[off + 1] << 8) | (p[off + 2] << 16);
                if (val & 0x800000) val |= 0xFF000000; // sign extend
                scratch[i] = val / 8388608.0;
            }
            processBlock(scratch, frames);
            for (int i = 0; i < n; i++) {
                const int32_t out = quantize24(scratch[i]);
                const int off = i * 3;
                p[off]     = out & 0xFF;
                p[off + 1] = (out >> 8) & 0xFF;
                p[off + 2] = (out >> 16) & 0xFF;
            }
        }
    }

    void process32(int32_t* samples, int count) {
        const int ch = channelCount;
        const int totalFrames = count / ch;
        for (int base = 0; base < totalFrames; base += BLOCK_FRAMES) {
            const int frames = (totalFrames - base) < BLOCK_FRAMES
                             ? (totalFrames - base) : BLOCK_FRAMES;
            const int n = frames * ch;
            int32_t* p = samples + (size_t)base * ch;
            for (int i = 0; i < n; i++) scratch[i] = p[i] * (1.0 / 2147483647.0);
            processBlock(scratch, frames);
            for (int i = 0; i < n; i++) p[i] = quantize32(scratch[i]);
        }
    }
};

#endif // EQ_PROCESSOR_H
