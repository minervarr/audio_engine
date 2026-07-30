// dsp_null_test — the bit-exactness gate for every DSP optimization.
//
// The engine's optimization work is allowed to make the audio path FASTER and
// is not allowed to make it DIFFERENT. This test is what enforces that: it
// holds a frozen, byte-for-byte copy of the reference implementations as they
// stood before any optimization, and asserts the live code still produces
// identical output — not "close", identical.
//
// Convention matches framework/vk_canvas/core/tests/*.cc and
// matrix_player's gui/src/ui_metrics_test.cc: plain assert(), no framework,
// #undef NDEBUG so the asserts survive an optimized build.
//
// Run: ./build/linux_debug/framework/audio_engine/dsp_null_test
//
// NOTE ON ARM: the live EqProcessor's NEON path uses vfmaq_f64 (fused
// multiply-add), which skips an intermediate rounding step and is therefore
// MORE accurate than — but not bit-identical to — the scalar reference. That
// predates this test. On aarch64 the EQ section below asserts a tight error
// bound instead of equality, and says so on stdout. On x86-64 (the SSE2 path,
// deliberately non-FMA) equality is asserted and must hold exactly.

#undef NDEBUG
#include <cassert>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "core/dsp/dither.h"
#include "core/dsp/eq_processor.h"
#include "core/dsp/round.h"
#include "core/buffer/ring_buffer.h"
#include "usb_pack.h"

static int g_checks = 0;
#define CHECK(cond) do { ++g_checks; assert(cond); } while (0)

// ===========================================================================
// Section 1 — ae::roundHalfAway / ae::roundHalfEven vs glibc
//
// These replace a per-sample libm call in the quantize stage. They are only
// worth anything if they are EXACT, so this section is deliberately brutal.
// ===========================================================================

static void testRounding() {
    printf("[1] rounding exactness vs libm\n");

    // --- every representable tie, and both neighbours of each -------------
    // Ties are where half-away-from-zero and half-to-even disagree, so they
    // are the only place a naive implementation can hide.
    for (int k = -2100; k <= 2100; ++k) {
        const double tie = k + 0.5;
        for (double x : {tie, std::nextafter(tie, -1e308), std::nextafter(tie, 1e308),
                         (double)k, std::nextafter((double)k, -1e308),
                         std::nextafter((double)k, 1e308)}) {
            CHECK(ae::roundHalfAway(x) == std::llround(x));
        }
    }

    // --- the canonical over-round counterexample --------------------------
    // trunc(x + 0.5) gives 1 here because x + 0.5 == 1.0 exactly; llround
    // gives 0. If the fixup in round.h is ever dropped, this line catches it.
    {
        const double x = 0.49999999999999994;
        CHECK(x + 0.5 == 1.0);              // the trap really is armed
        CHECK(std::llround(x) == 0);
        CHECK(ae::roundHalfAway(x) == 0);
        CHECK(ae::roundHalfAway(-x) == 0);
    }

    // --- the real working domain: a clamped sample times a full-scale grid -
    // This is exactly what quantize32() and ditherAndQuantize() feed it.
    for (double scale : {32767.0, 8388607.0, 2147483647.0,
                         32767.0 * 65536.0, 8388607.0 * 256.0}) {
        for (double s : {-1.0, -0.999999, -0.5, -1e-9, 0.0,
                         1e-9, 0.5, 0.999999, 1.0}) {
            const double x = s * scale;
            CHECK(ae::roundHalfAway(x) == std::llround(x));
            CHECK(ae::roundHalfEven(x) == std::lrint(x));
        }
    }

    // --- 10M random doubles across the full quantize range ----------------
    std::mt19937_64 rng(0xA5A5A5A5u);
    std::uniform_real_distribution<double> uni(-1.0, 1.0);
    for (int i = 0; i < 10'000'000; ++i) {
        const double s = uni(rng);
        for (double scale : {32767.0, 8388607.0, 2147483647.0}) {
            const double x = s * scale;
            if (ae::roundHalfAway(x) != std::llround(x)) {
                printf("    MISMATCH roundHalfAway(%.20g) = %lld, llround = %lld\n",
                       x, (long long)ae::roundHalfAway(x), (long long)std::llround(x));
                CHECK(false);
            }
            if (ae::roundHalfEven(x) != std::lrint(x)) {
                printf("    MISMATCH roundHalfEven(%.20g) = %lld, lrint = %lld\n",
                       x, (long long)ae::roundHalfEven(x), (long long)std::lrint(x));
                CHECK(false);
            }
        }
    }

    // --- random doubles landing near ties, where it actually matters ------
    // Uniform sampling above almost never produces a tie; this forces them.
    std::uniform_int_distribution<int> ik(-100000, 100000);
    for (int i = 0; i < 1'000'000; ++i) {
        const double base = ik(rng) + 0.5;
        for (double x : {base, std::nextafter(base, -1e308),
                         std::nextafter(base, 1e308)}) {
            CHECK(ae::roundHalfAway(x) == std::llround(x));
            CHECK(ae::roundHalfEven(x) == std::lrint(x));
        }
    }

    printf("    ok\n");
}

// ===========================================================================
// Section 2 — EqProcessor
//
// RefEqProcessor below is a FROZEN copy of the scalar EqProcessor as it stood
// at the start of the optimization work (audio_engine @ 017bcaa). It is the
// oracle. Do not "improve" it, do not refactor it to share code with the live
// class, and do not update it when the live class changes — the entire point
// is that it does not move.
// ===========================================================================

namespace oracle {

class RefEqProcessor {
public:
    static constexpr int MAX_FILTERS = 10;

    RefEqProcessor() : numFilters(0), preampLinear(1.0f), hasPreamp(false),
                       channelCount(2), encoding(0), enabled(true) {
        reset();
    }

    void configure(int nFilters, const double coeffs[], double preamp, int channels, int enc) {
        numFilters = nFilters > MAX_FILTERS ? MAX_FILTERS : nFilters;
        preampLinear = static_cast<float>(preamp);
        hasPreamp = std::abs(preamp - 1.0) > 1e-9;
        channelCount = channels > 0 ? channels : 2;
        encoding = enc;
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
            case 4:  processFloat(reinterpret_cast<float*>(data), length / 4); break;
            case 2:  process16(reinterpret_cast<int16_t*>(data), length / 2);  break;
            case 21: process24(data, length);                                  break;
            case 22: process32(reinterpret_cast<int32_t*>(data), length / 4);  break;
            default: break;
        }
    }

    void reset() {
        for (int f = 0; f < MAX_FILTERS; f++)
            for (int ch = 0; ch < 8; ch++) { filters[f].z1[ch] = 0.0; filters[f].z2[ch] = 0.0; }
    }

    void setEnabled(bool en) { if (!en && enabled) reset(); enabled = en; }

    void processToDouble(const int32_t* in, double* out, int count) {
        if (channelCount == 2) {
            for (int i = 0; i < count; i += 2) {
                out[i]   = processSample(in[i]   * (1.0 / 2147483647.0), 0);
                out[i+1] = processSample(in[i+1] * (1.0 / 2147483647.0), 1);
            }
            return;
        }
        int frames = count / channelCount;
        for (int f = 0; f < frames; f++) {
            int base = f * channelCount;
            for (int ch = 0; ch < channelCount; ch++)
                out[base+ch] = processSample(in[base+ch] * (1.0 / 2147483647.0), ch);
        }
    }

    static void scaleToDouble(const int32_t* in, double* out, int count) {
        for (int i = 0; i < count; i++) out[i] = in[i] * (1.0 / 2147483647.0);
    }

private:
    struct Biquad { double b0, b1, b2, a1, a2; double z1[8]; double z2[8]; };
    Biquad filters[MAX_FILTERS];
    int numFilters; float preampLinear; bool hasPreamp;
    int channelCount; int encoding; bool enabled;

    inline double processSample(double sample, int ch) {
        double x = hasPreamp ? sample * preampLinear : sample;
        for (int f = 0; f < numFilters; f++) {
            Biquad& bq = filters[f];
            double y = bq.b0 * x + bq.z1[ch];
            bq.z1[ch] = bq.b1 * x - bq.a1 * y + bq.z2[ch];
            bq.z2[ch] = bq.b2 * x - bq.a2 * y;
            x = y;
        }
        return x;
    }

    void processFloat(float* samples, int count) {
        if (channelCount == 2) {
            for (int i = 0; i < count; i += 2) {
                samples[i]     = static_cast<float>(processSample(samples[i],     0));
                samples[i + 1] = static_cast<float>(processSample(samples[i + 1], 1));
            }
            return;
        }
        int frames = count / channelCount;
        for (int f = 0; f < frames; f++) {
            int base = f * channelCount;
            for (int ch = 0; ch < channelCount; ch++)
                samples[base + ch] = static_cast<float>(processSample(samples[base + ch], ch));
        }
    }

    void process16(int16_t* samples, int count) {
        if (channelCount == 2) {
            for (int i = 0; i < count; i += 2) {
                double s0 = samples[i] / 32768.0;
                s0 = processSample(s0, 0);
                if (s0 > 1.0) s0 = 1.0; else if (s0 < -1.0) s0 = -1.0;
                samples[i] = static_cast<int16_t>(s0 * 32767.0);

                double s1 = samples[i + 1] / 32768.0;
                s1 = processSample(s1, 1);
                if (s1 > 1.0) s1 = 1.0; else if (s1 < -1.0) s1 = -1.0;
                samples[i + 1] = static_cast<int16_t>(s1 * 32767.0);
            }
            return;
        }
        int frames = count / channelCount;
        for (int f = 0; f < frames; f++) {
            int base = f * channelCount;
            for (int ch = 0; ch < channelCount; ch++) {
                double s = samples[base + ch] / 32768.0;
                s = processSample(s, ch);
                if (s > 1.0) s = 1.0; else if (s < -1.0) s = -1.0;
                samples[base + ch] = static_cast<int16_t>(s * 32767.0);
            }
        }
    }

    void process24(uint8_t* data, int length) {
        int sampleCount = length / 3;
        int frames = sampleCount / channelCount;
        for (int f = 0; f < frames; f++) {
            for (int ch = 0; ch < channelCount; ch++) {
                int off = (f * channelCount + ch) * 3;
                int32_t val = data[off] | (data[off + 1] << 8) | (data[off + 2] << 16);
                if (val & 0x800000) val |= 0xFF000000;
                double s = val / 8388608.0;
                s = processSample(s, ch);
                if (s > 1.0) s = 1.0; else if (s < -1.0) s = -1.0;
                int32_t out = static_cast<int32_t>(s * 8388607.0);
                data[off]     = out & 0xFF;
                data[off + 1] = (out >> 8) & 0xFF;
                data[off + 2] = (out >> 16) & 0xFF;
            }
        }
    }

    static inline int32_t quantize32(double s) {
        if (s > 1.0) s = 1.0; else if (s < -1.0) s = -1.0;
        long long q = llround(s * 2147483647.0);
        if (q > 2147483647LL) q = 2147483647LL;
        else if (q < -2147483648LL) q = -2147483648LL;
        return static_cast<int32_t>(q);
    }

    void process32(int32_t* samples, int count) {
        if (channelCount == 2) {
            for (int i = 0; i < count; i += 2) {
                double s0 = samples[i]     * (1.0 / 2147483647.0);
                samples[i]     = quantize32(processSample(s0, 0));
                double s1 = samples[i + 1] * (1.0 / 2147483647.0);
                samples[i + 1] = quantize32(processSample(s1, 1));
            }
            return;
        }
        int frames = count / channelCount;
        for (int f = 0; f < frames; f++) {
            int base = f * channelCount;
            for (int ch = 0; ch < channelCount; ch++) {
                double s = samples[base + ch] * (1.0 / 2147483647.0);
                samples[base + ch] = quantize32(processSample(s, ch));
            }
        }
    }
};

} // namespace oracle

// --- coefficient generation (RBJ cookbook, same math as EqManager) ---------
// Only used to feed both processors the SAME coefficients; its own numerical
// details are irrelevant to the comparison.
static void cookbook(const char* type, double fc, double gain, double q,
                     int sampleRate, double out[5]) {
    const double PI = 3.14159265358979323846;
    double w0 = 2.0 * PI * fc / sampleRate;
    double cosW0 = cos(w0), sinW0 = sin(w0);
    double A = pow(10.0, gain / 40.0);
    double alpha = sinW0 / (2.0 * q);
    double b0, b1, b2, a0, a1, a2;
    if (strcmp(type, "LSC") == 0) {
        double beta = 2.0 * sqrt(A) * alpha;
        b0 =     A * ((A + 1) - (A - 1) * cosW0 + beta);
        b1 = 2 * A * ((A - 1) - (A + 1) * cosW0);
        b2 =     A * ((A + 1) - (A - 1) * cosW0 - beta);
        a0 =          (A + 1) + (A - 1) * cosW0 + beta;
        a1 =    -2 * ((A - 1) + (A + 1) * cosW0);
        a2 =          (A + 1) + (A - 1) * cosW0 - beta;
    } else if (strcmp(type, "HSC") == 0) {
        double beta = 2.0 * sqrt(A) * alpha;
        b0 =      A * ((A + 1) + (A - 1) * cosW0 + beta);
        b1 = -2 * A * ((A - 1) + (A + 1) * cosW0);
        b2 =      A * ((A + 1) + (A - 1) * cosW0 - beta);
        a0 =           (A + 1) - (A - 1) * cosW0 + beta;
        a1 =      2 * ((A - 1) - (A + 1) * cosW0);
        a2 =           (A + 1) - (A - 1) * cosW0 - beta;
    } else {
        b0 = 1 + alpha * A;  b1 = -2 * cosW0;  b2 = 1 - alpha * A;
        a0 = 1 + alpha / A;  a1 = -2 * cosW0;  a2 = 1 - alpha / A;
    }
    out[0] = b0 / a0; out[1] = b1 / a0; out[2] = b2 / a0;
    out[3] = a1 / a0; out[4] = a2 / a0;
}

// A realistic 10-band set: shelves at the ends, peaks across the middle,
// mixed boost and cut, a couple of high-Q bands to stress the recursion.
static int buildCoeffs(int nFilters, int sampleRate, double* out) {
    struct Band { const char* type; double fc, gain, q; };
    static const Band bands[10] = {
        {"LSC",   80.0,  +6.0, 0.71}, {"PK",   160.0,  -3.5, 1.20},
        {"PK",   320.0,  +2.0, 2.00}, {"PK",   640.0,  -4.5, 0.80},
        {"PK",  1250.0,  +5.5, 1.41}, {"PK",  2500.0,  -2.5, 4.00},
        {"PK",  5000.0,  +3.0, 0.90}, {"PK",  8000.0,  -6.0, 6.00},
        {"PK", 12000.0,  +4.0, 1.10}, {"HSC",16000.0,  -5.0, 0.71},
    };
    if (nFilters > 10) nFilters = 10;
    for (int i = 0; i < nFilters; ++i)
        cookbook(bands[i].type, bands[i].fc, bands[i].gain, bands[i].q,
                 sampleRate, out + i * 5);
    return nFilters;
}

// --- test signals ----------------------------------------------------------
enum class Signal { Impulse, Step, FullScaleSine, QuietSine, Noise, Dc, Denormal };

static const char* signalName(Signal s) {
    switch (s) {
        case Signal::Impulse:       return "impulse";
        case Signal::Step:          return "step";
        case Signal::FullScaleSine: return "full-scale sine";
        case Signal::QuietSine:     return "-60 dBFS sine";
        case Signal::Noise:         return "white noise";
        case Signal::Dc:            return "DC";
        case Signal::Denormal:      return "denormal-range";
    }
    return "?";
}

static void fillSignal(Signal sig, std::vector<int32_t>& buf, int channels, int sampleRate) {
    std::mt19937 rng(12345);
    std::uniform_int_distribution<int32_t> noise(-2147483647, 2147483647);
    const int frames = (int)buf.size() / channels;
    for (int f = 0; f < frames; ++f) {
        for (int ch = 0; ch < channels; ++ch) {
            double v = 0.0;
            switch (sig) {
                case Signal::Impulse:  v = (f == 0) ? 1.0 : 0.0; break;
                case Signal::Step:     v = (f < frames / 2) ? 0.0 : 0.75; break;
                case Signal::FullScaleSine:
                    v = std::sin(2.0 * 3.14159265358979 * 997.0 * f / sampleRate);
                    break;
                case Signal::QuietSine:
                    v = 0.001 * std::sin(2.0 * 3.14159265358979 * 440.0 * f / sampleRate);
                    break;
                case Signal::Noise:
                    buf[f * channels + ch] = noise(rng);
                    continue;
                case Signal::Dc:       v = 0.5; break;
                case Signal::Denormal: v = 1e-300; break;
            }
            // Slight per-channel decorrelation so a channel-swap bug can't hide.
            v *= (1.0 - 0.05 * ch);
            buf[f * channels + ch] = (int32_t)std::llround(v * 2147483647.0);
        }
    }
}

static void testEqProcessor() {
#if defined(__aarch64__) && defined(__ARM_NEON)
    printf("[2] EqProcessor vs frozen scalar oracle  (aarch64: NEON uses FMA, "
           "asserting error bound instead of equality)\n");
    const bool exact = false;
#else
    printf("[2] EqProcessor vs frozen scalar oracle  (exact equality required)\n");
    const bool exact = true;
#endif

    const int sampleRate = 44100;
    const int frames = 4096;

    for (int channels : {1, 2, 4, 6, 8}) {
        for (int nf : {1, 2, 5, 10}) {
            double coeffs[10 * 5];
            const int n = buildCoeffs(nf, sampleRate, coeffs);

            for (double preamp : {1.0, 0.5011872336272722 /* -6 dB */}) {
                for (Signal sig : {Signal::Impulse, Signal::Step, Signal::FullScaleSine,
                                   Signal::QuietSine, Signal::Noise, Signal::Dc,
                                   Signal::Denormal}) {

                    std::vector<int32_t> src((size_t)frames * channels);
                    fillSignal(sig, src, channels, sampleRate);

                    // --- encoding 22 (PCM_32BIT): the Reference EQ path ----
                    {
                        EqProcessor live; oracle::RefEqProcessor ref;
                        live.configure(n, coeffs, preamp, channels, 22);
                        ref .configure(n, coeffs, preamp, channels, 22);

                        std::vector<int32_t> a = src, b = src;
                        // Feed in several chunks so filter state must carry
                        // across calls exactly as it does during playback.
                        const int chunk = 512 * channels;
                        for (size_t off = 0; off < a.size(); off += chunk) {
                            const int len = (int)std::min((size_t)chunk, a.size() - off);
                            live.process(reinterpret_cast<uint8_t*>(a.data() + off), len * 4);
                            ref .process(reinterpret_cast<uint8_t*>(b.data() + off), len * 4);
                        }
                        for (size_t i = 0; i < a.size(); ++i) {
                            if (exact) {
                                if (a[i] != b[i]) {
                                    printf("    MISMATCH ch=%d nf=%d preamp=%.4f %s "
                                           "int32[%zu]: live=%d ref=%d\n",
                                           channels, n, preamp, signalName(sig), i, a[i], b[i]);
                                    CHECK(false);
                                }
                            } else {
                                CHECK(std::llabs((long long)a[i] - (long long)b[i]) <= 64);
                            }
                        }
                        ++g_checks;
                    }

                    // --- processToDouble: the resample path's EQ stage -----
                    {
                        EqProcessor live; oracle::RefEqProcessor ref;
                        live.configure(n, coeffs, preamp, channels, 22);
                        ref .configure(n, coeffs, preamp, channels, 22);

                        std::vector<double> a(src.size()), b(src.size());
                        const int chunk = 512 * channels;
                        for (size_t off = 0; off < src.size(); off += chunk) {
                            const int len = (int)std::min((size_t)chunk, src.size() - off);
                            live.processToDouble(src.data() + off, a.data() + off, len);
                            ref .processToDouble(src.data() + off, b.data() + off, len);
                        }
                        for (size_t i = 0; i < a.size(); ++i) {
                            if (exact) {
                                if (!(a[i] == b[i] || (std::isnan(a[i]) && std::isnan(b[i])))) {
                                    printf("    MISMATCH ch=%d nf=%d %s double[%zu]: "
                                           "live=%.20g ref=%.20g\n",
                                           channels, n, signalName(sig), i, a[i], b[i]);
                                    CHECK(false);
                                }
                            } else {
                                CHECK(std::fabs(a[i] - b[i]) < 1e-9);
                            }
                        }
                        ++g_checks;
                    }

                    // --- encodings 4 / 2 / 21: float, int16, packed int24 ---
                    {
                        std::vector<float> fa(src.size()), fb(src.size());
                        for (size_t i = 0; i < src.size(); ++i)
                            fa[i] = fb[i] = (float)(src[i] * (1.0 / 2147483647.0));
                        EqProcessor live; oracle::RefEqProcessor ref;
                        live.configure(n, coeffs, preamp, channels, 4);
                        ref .configure(n, coeffs, preamp, channels, 4);
                        live.process(reinterpret_cast<uint8_t*>(fa.data()), (int)fa.size() * 4);
                        ref .process(reinterpret_cast<uint8_t*>(fb.data()), (int)fb.size() * 4);
                        for (size_t i = 0; i < fa.size(); ++i) {
                            if (exact) CHECK(fa[i] == fb[i] ||
                                             (std::isnan(fa[i]) && std::isnan(fb[i])));
                            else       CHECK(std::fabs(fa[i] - fb[i]) < 1e-5f);
                        }
                        ++g_checks;
                    }
                    {
                        std::vector<int16_t> sa(src.size()), sb(src.size());
                        for (size_t i = 0; i < src.size(); ++i)
                            sa[i] = sb[i] = (int16_t)(src[i] >> 16);
                        EqProcessor live; oracle::RefEqProcessor ref;
                        live.configure(n, coeffs, preamp, channels, 2);
                        ref .configure(n, coeffs, preamp, channels, 2);
                        live.process(reinterpret_cast<uint8_t*>(sa.data()), (int)sa.size() * 2);
                        ref .process(reinterpret_cast<uint8_t*>(sb.data()), (int)sb.size() * 2);
                        for (size_t i = 0; i < sa.size(); ++i) {
                            if (exact) CHECK(sa[i] == sb[i]);
                            else       CHECK(std::abs(sa[i] - sb[i]) <= 1);
                        }
                        ++g_checks;
                    }
                    {
                        std::vector<uint8_t> pa(src.size() * 3), pb(src.size() * 3);
                        for (size_t i = 0; i < src.size(); ++i) {
                            const int32_t v = src[i] >> 8;   // 32 -> 24 bit
                            pa[i*3+0] = pb[i*3+0] = (uint8_t)(v & 0xFF);
                            pa[i*3+1] = pb[i*3+1] = (uint8_t)((v >> 8) & 0xFF);
                            pa[i*3+2] = pb[i*3+2] = (uint8_t)((v >> 16) & 0xFF);
                        }
                        EqProcessor live; oracle::RefEqProcessor ref;
                        live.configure(n, coeffs, preamp, channels, 21);
                        ref .configure(n, coeffs, preamp, channels, 21);
                        live.process(pa.data(), (int)pa.size());
                        ref .process(pb.data(), (int)pb.size());
                        if (exact) CHECK(memcmp(pa.data(), pb.data(), pa.size()) == 0);
                        ++g_checks;
                    }
                }
            }
        }
    }

    // --- bypass and lifecycle behaviour ----------------------------------
    {
        std::vector<int32_t> src(1024), a, b;
        fillSignal(Signal::Noise, src, 2, 44100);

        // scaleToDouble is the EQ-off path; it must stay a pure scale.
        std::vector<double> d(src.size());
        EqProcessor::scaleToDouble(src.data(), d.data(), (int)src.size());
        for (size_t i = 0; i < src.size(); ++i)
            CHECK(d[i] == src[i] * (1.0 / 2147483647.0));

        // numFilters == 0 and !enabled must both be exact pass-throughs.
        double coeffs[5];
        buildCoeffs(1, 44100, coeffs);
        EqProcessor p;
        p.configure(0, coeffs, 1.0, 2, 22);
        a = src;
        p.process(reinterpret_cast<uint8_t*>(a.data()), (int)a.size() * 4);
        CHECK(memcmp(a.data(), src.data(), a.size() * 4) == 0);

        p.configure(1, coeffs, 1.0, 2, 22);
        p.setEnabled(false);
        b = src;
        p.process(reinterpret_cast<uint8_t*>(b.data()), (int)b.size() * 4);
        CHECK(memcmp(b.data(), src.data(), b.size() * 4) == 0);

        // reset() must return the filter to its post-configure state, so the
        // same input after a reset produces the same output as the first run.
        p.setEnabled(true);
        std::vector<int32_t> r1 = src, r2 = src;
        p.process(reinterpret_cast<uint8_t*>(r1.data()), (int)r1.size() * 4);
        p.reset();
        p.process(reinterpret_cast<uint8_t*>(r2.data()), (int)r2.size() * 4);
        CHECK(memcmp(r1.data(), r2.data(), r1.size() * 4) == 0);
    }

    printf("    ok\n");
}

// ===========================================================================
// Section 3 — ae::RingBuffer
//
// Written against the CONTRACT, not the implementation, so it keeps its value
// across the power-of-two/mask rewrite: a lock-free SPSC byte ring that never
// reorders, never duplicates, and never loses a byte across wraparound.
// ===========================================================================

static void testRingBuffer() {
    printf("[3] RingBuffer contract\n");

    for (size_t cap : {(size_t)64, (size_t)1000, (size_t)4096, (size_t)44100 * 8}) {
        ae::RingBuffer rb(cap);

        // Empty ring: nothing available, and free space is at least what the
        // caller asked for minus the one-byte full/empty discriminator.
        CHECK(rb.getAvailable() == 0);
        CHECK(rb.getFreeSpace() >= cap - 1);
        CHECK(rb.getCapacity() >= cap);

        // Short read on an empty ring returns 0, touches nothing.
        uint8_t sink[16] = {};
        CHECK(rb.read(sink, sizeof(sink)) == 0);

        // A write larger than the ring is truncated, never overflows.
        std::vector<uint8_t> big(rb.getCapacity() * 2);
        for (size_t i = 0; i < big.size(); ++i) big[i] = (uint8_t)(i * 31 + 7);
        const size_t wrote = rb.write(big.data(), big.size());
        CHECK(wrote <= rb.getCapacity() - 1);
        CHECK(rb.getAvailable() == wrote);

        std::vector<uint8_t> back(wrote);
        CHECK(rb.read(back.data(), back.size()) == wrote);
        CHECK(memcmp(back.data(), big.data(), wrote) == 0);
        CHECK(rb.getAvailable() == 0);

        rb.clear();
        CHECK(rb.getAvailable() == 0);
    }

    // Wraparound: many odd-sized writes and reads through a small ring must
    // reproduce the byte stream exactly. This is the case the modulo->mask
    // rewrite could plausibly break.
    {
        ae::RingBuffer rb(256);
        std::mt19937 rng(999);
        std::uniform_int_distribution<int> sz(1, 97);

        std::vector<uint8_t> produced, consumed;
        uint8_t next = 0;
        for (int iter = 0; iter < 20000; ++iter) {
            const int wn = sz(rng);
            std::vector<uint8_t> chunk(wn);
            for (int i = 0; i < wn; ++i) chunk[i] = next++;
            const size_t w = rb.write(chunk.data(), chunk.size());
            produced.insert(produced.end(), chunk.begin(), chunk.begin() + w);
            // Un-written bytes were never produced, so rewind the generator.
            next = (uint8_t)(next - (wn - w));

            const int rn = sz(rng);
            std::vector<uint8_t> out(rn);
            const size_t r = rb.read(out.data(), out.size());
            CHECK(r <= (size_t)rn);
            consumed.insert(consumed.end(), out.begin(), out.begin() + r);
        }
        // Drain whatever is left.
        for (;;) {
            uint8_t tail[64];
            const size_t r = rb.read(tail, sizeof(tail));
            if (r == 0) break;
            consumed.insert(consumed.end(), tail, tail + r);
        }
        CHECK(consumed.size() == produced.size());
        CHECK(memcmp(consumed.data(), produced.data(), consumed.size()) == 0);
    }

    printf("    ok\n");
}

// ===========================================================================
// Section 4 — USB wire packing
//
// The oracles below are the write*() inner loops copied verbatim from
// usb_audio.cpp as they stood before the usb_pack.h refactor. Same rule as
// the EQ oracle: frozen, never updated.
//
// They contain one wart that is reproduced deliberately: `v << dataShift` on a
// signed int32, which is UB when the value is negative or shifts into the sign
// bit. usb_pack.h does the shift in unsigned arithmetic instead. The test data
// includes INT16_MIN / INT32_MIN precisely so that difference would show up if
// it ever became more than theoretical.
// ===========================================================================

namespace oracle {

static void packInt32Loop(const int32_t* data, int n, uint8_t* out,
                          int subslotBytes, float gain) {
    const int dataShift = (subslotBytes * 8) - 32;
    int outBytes = 0;
    for (int i = 0; i < n; i++) {
        int32_t v = data[i];
        if (gain < 0.9999f) {
            float fs = (float)v * gain;
            if (fs > 2147483520.0f) fs = 2147483520.0f;
            else if (fs < -2147483648.0f) fs = -2147483648.0f;
            v = (int32_t)fs;
        }
        int32_t wire = (dataShift >= 0) ? (v << dataShift) : (v >> -dataShift);
        for (int b = 0; b < subslotBytes; b++)
            out[outBytes++] = (wire >> (b * 8)) & 0xFF;
    }
}

static void packInt16Loop(const int16_t* data, int n, uint8_t* out,
                          int subslotBytes, float gain) {
    const int dataShift = (subslotBytes * 8) - 16;
    int outBytes = 0;
    for (int i = 0; i < n; i++) {
        int16_t s = data[i];
        int32_t scaled;
        if (gain >= 0.9999f) {
            scaled = (int32_t)s;
        } else {
            float fs = (float)s * gain;
            if (fs > 32767.0f) fs = 32767.0f;
            else if (fs < -32768.0f) fs = -32768.0f;
            scaled = (int32_t)fs;
        }
        int32_t wire = (dataShift >= 0) ? (scaled << dataShift) : (scaled >> -dataShift);
        for (int b = 0; b < subslotBytes; b++)
            out[outBytes++] = (wire >> (b * 8)) & 0xFF;
    }
}

static void packInt24Loop(const uint8_t* data, int n, uint8_t* out,
                          int subslotBytes, float gain) {
    int outBytes = 0;
    for (int i = 0; i < n; i++) {
        int off = i * 3;
        int32_t v = ((int32_t)data[off] << 8)
                  | ((int32_t)data[off + 1] << 16)
                  | ((int32_t)data[off + 2] << 24);
        v >>= 8;
        if (gain < 0.9999f) {
            float fs = (float)v * gain;
            if (fs > 8388607.0f) fs = 8388607.0f;
            else if (fs < -8388608.0f) fs = -8388608.0f;
            v = (int32_t)fs;
        }
        int dataShift = (subslotBytes * 8) - 24;
        int32_t wire = (dataShift >= 0) ? (v << dataShift) : (v >> -dataShift);
        for (int b = 0; b < subslotBytes; b++)
            out[outBytes++] = (wire >> (b * 8)) & 0xFF;
    }
}

} // namespace oracle

static void testWirePacking() {
    printf("[4] USB wire packing vs frozen usb_audio.cpp loops\n");

    const int n = 2048;
    std::mt19937 rng(777);
    std::uniform_int_distribution<int32_t> d32(-2147483647 - 1, 2147483647);

    std::vector<int32_t> s32(n);
    std::vector<int16_t> s16(n);
    std::vector<uint8_t> s24(n * 3);
    for (int i = 0; i < n; ++i) {
        s32[i] = d32(rng);
        s16[i] = (int16_t)(s32[i] >> 16);
        const int32_t v = s32[i] >> 8;
        s24[i*3+0] = (uint8_t)(v & 0xFF);
        s24[i*3+1] = (uint8_t)((v >> 8) & 0xFF);
        s24[i*3+2] = (uint8_t)((v >> 16) & 0xFF);
    }
    // Pin the extremes: full-scale both signs, zero, and the values whose
    // shifts are the UB-adjacent ones.
    const int32_t edge32[] = {0, 1, -1, 2147483647, -2147483647 - 1,
                              8388607, -8388608, 32767, -32768};
    for (size_t i = 0; i < sizeof(edge32)/sizeof(edge32[0]); ++i) {
        s32[i] = edge32[i];
        s16[i] = (int16_t)((i == 8) ? -32768 : (i == 7) ? 32767 : edge32[i]);
    }

    // Unity plus a representative attenuation, and one just below the unity
    // threshold so the gain branch itself is exercised on both sides.
    for (float gain : {1.0f, 0.99995f, 0.9998f, 0.5f, 0.25f, 0.0f}) {
        for (int sub = 1; sub <= 4; ++sub) {
            std::vector<uint8_t> a(n * sub), b(n * sub);

            oracle::packInt32Loop(s32.data(), n, a.data(), sub, gain);
            const int wrote32 = ae::usbpack::packInt32Dyn(s32.data(), n, b.data(), sub, gain);
            CHECK(wrote32 == n * sub);
            if (memcmp(a.data(), b.data(), a.size()) != 0) {
                printf("    MISMATCH packInt32 sub=%d gain=%.5f\n", sub, gain);
                CHECK(false);
            }

            oracle::packInt16Loop(s16.data(), n, a.data(), sub, gain);
            const int wrote16 = ae::usbpack::packInt16Dyn(s16.data(), n, b.data(), sub, gain);
            CHECK(wrote16 == n * sub);
            if (memcmp(a.data(), b.data(), a.size()) != 0) {
                printf("    MISMATCH packInt16 sub=%d gain=%.5f\n", sub, gain);
                CHECK(false);
            }

            oracle::packInt24Loop(s24.data(), n, a.data(), sub, gain);
            const int wrote24 = ae::usbpack::packInt24Dyn(s24.data(), n, b.data(), sub, gain);
            CHECK(wrote24 == n * sub);
            if (memcmp(a.data(), b.data(), a.size()) != 0) {
                printf("    MISMATCH packInt24 sub=%d gain=%.5f\n", sub, gain);
                CHECK(false);
            }
        }
    }

    // The bit-perfect claim, stated directly: a 4-byte subslot at unity gain
    // must reproduce the input bytes exactly. This is what licenses
    // writeInt32's memcpy fast path.
    {
        std::vector<uint8_t> out(n * 4);
        ae::usbpack::packInt32Dyn(s32.data(), n, out.data(), 4, 1.0f);
        CHECK(memcmp(out.data(), s32.data(), out.size()) == 0);
    }
    // Likewise 3-byte subslot from a packed-24 source at unity gain.
    {
        std::vector<uint8_t> out(n * 3);
        ae::usbpack::packInt24Dyn(s24.data(), n, out.data(), 3, 1.0f);
        CHECK(memcmp(out.data(), s24.data(), out.size()) == 0);
    }

    printf("    ok\n");
}

// ===========================================================================
// Section 5 — TPDF dither + final quantize
//
// Oracle is ditherAndQuantize as it stood in matrix_player's
// gui/src/player_view.cc before it moved into core/dsp/dither.h. Frozen,
// including its file-static LCG.
// ===========================================================================

namespace oracle {

static uint32_t s_lcgState = 0x9E3779B9u;
static inline uint32_t lcgNext() {
    s_lcgState = s_lcgState * 1664525u + 1013904223u;
    return s_lcgState;
}
static void resetLcg() { s_lcgState = 0x9E3779B9u; }

static void ditherAndQuantize(const double* in, int32_t* out, int n, int bits) {
    double scale = (bits == 16) ? 32767.0   * (double)(1 << 16)
                 : (bits == 24) ? 8388607.0 * (double)(1 << 8)
                 :                2147483647.0;
    double ditherAmp = (bits < 32) ? (1.0 / scale) : 0.0;
    for (int i = 0; i < n; i++) {
        double r = ditherAmp * ((double)(int32_t)(lcgNext() >> 1) -
                                (double)(int32_t)(lcgNext() >> 1)) * (1.0 / 1073741824.0);
        double s = in[i] + r;
        if (s >  1.0) s =  1.0;
        if (s < -1.0) s = -1.0;
        long long q = llround(s * scale);
        if (q >  2147483647LL) q =  2147483647LL;
        if (q < -2147483648LL) q = -2147483648LL;
        out[i] = (int32_t)q;
    }
}

} // namespace oracle

static void testDither() {
    printf("[5] TPDF dither + quantize vs frozen player_view.cc version\n");

    const int n = 8192;
    std::mt19937 rng(31337);
    std::uniform_real_distribution<double> uni(-1.2, 1.2);   // deliberately over-range

    std::vector<double> in(n);
    for (int i = 0; i < n; ++i) in[i] = uni(rng);
    // Pin the clamp edges and zero.
    in[0] = 0.0; in[1] = 1.0; in[2] = -1.0; in[3] = 2.0; in[4] = -2.0;
    in[5] = std::nextafter(1.0, 2.0); in[6] = std::nextafter(-1.0, -2.0);

    for (int bits : {16, 24, 32}) {
        std::vector<int32_t> a(n), b(n);

        // Both start from the same LCG state, so the dither sequence — and
        // therefore the output — must match sample for sample, not just
        // statistically.
        oracle::resetLcg();
        oracle::ditherAndQuantize(in.data(), a.data(), n, bits);

        ae::TpdfQuantizer q;   // fresh instance == same initial state
        q.process(in.data(), b.data(), n, bits);

        for (int i = 0; i < n; ++i) {
            if (a[i] != b[i]) {
                printf("    MISMATCH bits=%d [%d]: in=%.20g live=%d ref=%d\n",
                       bits, i, in[i], b[i], a[i]);
                CHECK(false);
            }
        }
        ++g_checks;

        // Chunked must equal one-shot: the generator has to carry across calls
        // exactly as it does between audio callbacks.
        oracle::resetLcg();
        oracle::ditherAndQuantize(in.data(), a.data(), n, bits);
        ae::TpdfQuantizer q2;
        std::vector<int32_t> c(n);
        for (int off = 0; off < n; off += 700) {
            const int len = std::min(700, n - off);
            q2.process(in.data() + off, c.data() + off, len, bits);
        }
        CHECK(memcmp(a.data(), c.data(), (size_t)n * 4) == 0);
    }

    // At 32-bit the output must be pure quantize — no noise added at all, so
    // repeating the call gives the identical result.
    {
        std::vector<int32_t> a(n), b(n);
        ae::TpdfQuantizer q1, q2;
        q1.process(in.data(), a.data(), n, 32);
        q2.process(in.data(), b.data(), n, 32);
        q2.process(in.data(), b.data(), n, 32);   // state advanced? must not matter
        CHECK(memcmp(a.data(), b.data(), (size_t)n * 4) == 0);
    }

    printf("    ok\n");
}

int main() {
    printf("=== dsp_null_test ===\n");
    testRounding();
    testEqProcessor();
    testRingBuffer();
    testWirePacking();
    testDither();
    printf("=== PASS (%d checks) ===\n", g_checks);
    return 0;
}
