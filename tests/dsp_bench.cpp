// dsp_bench — ns/sample for every hot path the optimization work touches.
//
// Companion to dsp_null_test: that one proves the audio did not change, this
// one proves the change was worth making. Run it before and after each phase
// and compare. It is a measurement tool, not a test — it asserts nothing.
//
// Run: ./build/linux_debug/framework/audio_engine/dsp_bench
//
// Note: built Debug-only alongside the null test, but the file itself is
// compiled -O3 (see CMakeLists) — benchmarking unoptimized code would measure
// nothing useful. The engine headers it exercises are header-only, so they get
// the same treatment they get in the real Release build.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "core/dsp/audio_convert.h"   // DitherLCG, as writeFloat32 uses it
#include "core/dsp/dither.h"
#include "core/dsp/eq_processor.h"
#include "core/dsp/round.h"
#include "core/buffer/ring_buffer.h"
#include "usb_pack.h"

// Keep the optimizer from deleting work whose result is unused.
static volatile uint64_t g_sink = 0;
template <typename T> static inline void consume(const T& v) {
    g_sink += (uint64_t)v;
}
static inline void consumeBuf(const void* p, size_t bytes) {
    const uint8_t* b = (const uint8_t*)p;
    uint64_t acc = 0;
    for (size_t i = 0; i < bytes; i += 64) acc += b[i];
    g_sink += acc;
}

using Clock = std::chrono::steady_clock;

struct Result { double nsPerSample; };

// Runs fn repeatedly until at least minMs of wall time has elapsed, then
// reports the best (least noisy) per-sample cost observed.
template <typename F>
static Result bench(int samplesPerCall, F&& fn, int minMs = 300) {
    // Warm up caches and let the CPU clock ramp.
    for (int i = 0; i < 3; ++i) fn();

    double best = 1e300;
    const auto deadline = Clock::now() + std::chrono::milliseconds(minMs);
    do {
        const auto t0 = Clock::now();
        const int reps = 8;
        for (int i = 0; i < reps; ++i) fn();
        const auto t1 = Clock::now();
        const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
        best = std::min(best, ns / (reps * (double)samplesPerCall));
    } while (Clock::now() < deadline);
    return {best};
}

static void report(const char* name, Result r) {
    printf("  %-46s %8.3f ns/sample\n", name, r.nsPerSample);
}

// --- shared test data -------------------------------------------------------
static constexpr int kFrames   = 4096;
static constexpr int kChannels = 2;
static constexpr int kSamples  = kFrames * kChannels;

static void cookbookPeak(double fc, double gain, double q, int sr, double out[5]) {
    const double PI = 3.14159265358979323846;
    double w0 = 2.0 * PI * fc / sr, cosW0 = cos(w0), sinW0 = sin(w0);
    double A = pow(10.0, gain / 40.0), alpha = sinW0 / (2.0 * q);
    double b0 = 1 + alpha * A, b1 = -2 * cosW0, b2 = 1 - alpha * A;
    double a0 = 1 + alpha / A, a1 = -2 * cosW0, a2 = 1 - alpha / A;
    out[0] = b0/a0; out[1] = b1/a0; out[2] = b2/a0; out[3] = a1/a0; out[4] = a2/a0;
}

static void buildCoeffs(int n, double* out) {
    for (int i = 0; i < n; ++i)
        cookbookPeak(60.0 * std::pow(2.0, i * 0.8), (i % 2) ? -4.0 : +4.0,
                     1.0 + 0.2 * i, 44100, out + i * 5);
}

// ===========================================================================
// The USB wire-packing inner loops, as they stand in
// backends/usb/usb_audio.cpp today. Copied here rather than called, because
// they live inside UsbAudioDriver and need an open device to reach. Phase 1
// factors them into backends/usb/usb_pack.h; when it does, add the factored
// version alongside these so the two can be compared directly.
// ===========================================================================

namespace baseline {

// writeFloat32's inner loop (usb_audio.cpp:1859-1894), unity gain, no fade.
static void packFloat32(const float* data, int batch, uint8_t* convBuf,
                        int subslotBytes, int bitDepth, float gain) {
    const int padBits = std::max(0, subslotBytes * 8 - bitDepth);
    static DitherLCG rng;
    int outBytes = 0;
    for (int i = 0; i < batch; i++) {
        double sd = std::max(-1.0, std::min(1.0, (double)(data[i] * gain)));
        int32_t v;
        switch (bitDepth) {
            case 24: v = (int32_t)lrint(sd * 8388607.0);    break;
            case 32: v = (int32_t)lrint(sd * 2147483647.0); break;
            case 16:
            default: {
                double scaled = sd * 32767.0 + rng.nextTPDF();
                int32_t q = (int32_t)lrint(scaled);
                if (q > 32767) q = 32767; else if (q < -32768) q = -32768;
                v = q;
                break;
            }
        }
        int32_t wire = v << padBits;
        for (int b = 0; b < subslotBytes; b++)
            convBuf[outBytes++] = (wire >> (b * 8)) & 0xFF;
    }
}

// writeInt32's inner loop (usb_audio.cpp:2052-2065).
static void packInt32(const int32_t* data, int batch, uint8_t* convBuf,
                      int subslotBytes, float gain) {
    const int dataShift = (subslotBytes * 8) - 32;
    int outBytes = 0;
    for (int i = 0; i < batch; i++) {
        int32_t v = data[i];
        if (gain < 0.9999f) {
            float fs = (float)v * gain;
            if (fs > 2147483520.0f) fs = 2147483520.0f;
            else if (fs < -2147483648.0f) fs = -2147483648.0f;
            v = (int32_t)fs;
        }
        int32_t wire = (dataShift >= 0) ? (v << dataShift) : (v >> -dataShift);
        for (int b = 0; b < subslotBytes; b++)
            convBuf[outBytes++] = (wire >> (b * 8)) & 0xFF;
    }
}

// writeInt24Packed's inner loop (usb_audio.cpp:1988-2014).
static void packInt24(const uint8_t* data, int batch, uint8_t* convBuf,
                      int subslotBytes, int bitDepth, float gain) {
    const int padBits = std::max(0, subslotBytes * 8 - bitDepth);
    (void)padBits;
    int outBytes = 0;
    for (int i = 0; i < batch; i++) {
        const int off = i * 3;
        int32_t v = ((int32_t)data[off] << 8) | ((int32_t)data[off+1] << 16)
                  | ((int32_t)data[off+2] << 24);
        v >>= 8;
        if (gain < 0.9999f) {
            float fs = (float)v * gain;
            if (fs > 8388607.0f) fs = 8388607.0f;
            else if (fs < -8388608.0f) fs = -8388608.0f;
            v = (int32_t)fs;
        }
        const int dataShift = (subslotBytes * 8) - 24;
        int32_t wire = (dataShift >= 0) ? (v << dataShift) : (v >> -dataShift);
        for (int b = 0; b < subslotBytes; b++)
            convBuf[outBytes++] = (wire >> (b * 8)) & 0xFF;
    }
}

// ditherAndQuantize, from matrix_player's gui/src/player_view.cc:38 — the
// Reference EQ resample path's per-sample output stage.
static uint32_t s_lcg = 0x9E3779B9u;
static inline uint32_t lcgNext() { s_lcg = s_lcg * 1664525u + 1013904223u; return s_lcg; }

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

} // namespace baseline

int main() {
    printf("=== dsp_bench ===  %d frames x %d ch per call\n\n", kFrames, kChannels);

    std::mt19937 rng(4242);
    std::uniform_real_distribution<double> uni(-1.0, 1.0);

    std::vector<int32_t> i32(kSamples);
    std::vector<float>   f32(kSamples);
    std::vector<double>  f64(kSamples);
    std::vector<uint8_t> i24(kSamples * 3);
    for (int i = 0; i < kSamples; ++i) {
        const double v = uni(rng) * 0.8;
        f64[i] = v;
        f32[i] = (float)v;
        i32[i] = (int32_t)std::llround(v * 2147483647.0);
        const int32_t v24 = i32[i] >> 8;
        i24[i*3+0] = (uint8_t)(v24 & 0xFF);
        i24[i*3+1] = (uint8_t)((v24 >> 8) & 0xFF);
        i24[i*3+2] = (uint8_t)((v24 >> 16) & 0xFF);
    }

    // ---------------------------------------------------------------- EQ ---
    printf("EQ biquad cascade (stereo, encoding 22 / PCM_32BIT):\n");
    for (int nf : {1, 5, 10}) {
        double coeffs[10 * 5];
        buildCoeffs(nf, coeffs);
        EqProcessor eq;
        eq.configure(nf, coeffs, 1.0, kChannels, 22);
        std::vector<int32_t> work(kSamples);
        char label[64];
        snprintf(label, sizeof(label), "process32          %2d filter(s)", nf);
        report(label, bench(kSamples, [&] {
            memcpy(work.data(), i32.data(), work.size() * 4);
            eq.process(reinterpret_cast<uint8_t*>(work.data()), (int)work.size() * 4);
            consumeBuf(work.data(), work.size() * 4);
        }));
    }
    for (int nf : {1, 5, 10}) {
        double coeffs[10 * 5];
        buildCoeffs(nf, coeffs);
        EqProcessor eq;
        eq.configure(nf, coeffs, 1.0, kChannels, 22);
        std::vector<double> outd(kSamples);
        char label[64];
        snprintf(label, sizeof(label), "processToDouble    %2d filter(s)", nf);
        report(label, bench(kSamples, [&] {
            eq.processToDouble(i32.data(), outd.data(), kSamples);
            consumeBuf(outd.data(), outd.size() * 8);
        }));
    }

    // ------------------------------------------------------ wire packing ---
    printf("\nUSB wire packing (baseline: current usb_audio.cpp inner loops):\n");
    {
        std::vector<uint8_t> conv(kSamples * 4);
        report("writeFloat32 loop  16-bit / 2-byte subslot", bench(kSamples, [&] {
            baseline::packFloat32(f32.data(), kSamples, conv.data(), 2, 16, 1.0f);
            consumeBuf(conv.data(), kSamples * 2);
        }));
        report("writeFloat32 loop  24-bit / 3-byte subslot", bench(kSamples, [&] {
            baseline::packFloat32(f32.data(), kSamples, conv.data(), 3, 24, 1.0f);
            consumeBuf(conv.data(), kSamples * 3);
        }));
        report("writeFloat32 loop  32-bit / 4-byte subslot", bench(kSamples, [&] {
            baseline::packFloat32(f32.data(), kSamples, conv.data(), 4, 32, 1.0f);
            consumeBuf(conv.data(), kSamples * 4);
        }));
        report("writeInt32 loop    4-byte subslot, unity gain", bench(kSamples, [&] {
            baseline::packInt32(i32.data(), kSamples, conv.data(), 4, 1.0f);
            consumeBuf(conv.data(), kSamples * 4);
        }));
        report("writeInt32 loop    3-byte subslot, unity gain", bench(kSamples, [&] {
            baseline::packInt32(i32.data(), kSamples, conv.data(), 3, 1.0f);
            consumeBuf(conv.data(), kSamples * 3);
        }));
        report("writeInt24Packed   3-byte subslot, unity gain", bench(kSamples, [&] {
            baseline::packInt24(i24.data(), kSamples, conv.data(), 3, 24, 1.0f);
            consumeBuf(conv.data(), kSamples * 3);
        }));
        report("memcpy reference   (the achievable floor)", bench(kSamples, [&] {
            memcpy(conv.data(), i32.data(), kSamples * 4);
            consumeBuf(conv.data(), kSamples * 4);
        }));

        printf("\nUSB wire packing (usb_pack.h — compile-time subslot width):\n");
        report("packInt32Dyn       16-bit / 2-byte subslot", bench(kSamples, [&] {
            ae::usbpack::packInt32Dyn(i32.data(), kSamples, conv.data(), 2, 1.0f);
            consumeBuf(conv.data(), kSamples * 2);
        }));
        report("packInt32Dyn       3-byte subslot, unity gain", bench(kSamples, [&] {
            ae::usbpack::packInt32Dyn(i32.data(), kSamples, conv.data(), 3, 1.0f);
            consumeBuf(conv.data(), kSamples * 3);
        }));
        report("packInt32Dyn       4-byte subslot, unity gain", bench(kSamples, [&] {
            ae::usbpack::packInt32Dyn(i32.data(), kSamples, conv.data(), 4, 1.0f);
            consumeBuf(conv.data(), kSamples * 4);
        }));
        report("packInt32Dyn       4-byte subslot, 0.5 gain", bench(kSamples, [&] {
            ae::usbpack::packInt32Dyn(i32.data(), kSamples, conv.data(), 4, 0.5f);
            consumeBuf(conv.data(), kSamples * 4);
        }));
        report("packInt24Dyn       3-byte subslot, unity gain", bench(kSamples, [&] {
            ae::usbpack::packInt24Dyn(i24.data(), kSamples, conv.data(), 3, 1.0f);
            consumeBuf(conv.data(), kSamples * 3);
        }));
        report("writeInt32 bit-perfect path (ring write only)", bench(kSamples, [&] {
            // What writeInt32 now does for a 4-byte subslot at unity gain: no
            // staging buffer, no per-sample work, straight into the ring.
            memcpy(conv.data(), i32.data(), kSamples * 4);
            consumeBuf(conv.data(), kSamples * 4);
        }));
    }

    // --------------------------------------------------- dither/quantize ---
    printf("\nReference EQ output stage (ditherAndQuantize):\n");
    {
        std::vector<int32_t> out(kSamples);
        for (int bits : {16, 24, 32}) {
            char label[64];
            snprintf(label, sizeof(label), "baseline           %d-bit target", bits);
            report(label, bench(kSamples, [&] {
                baseline::ditherAndQuantize(f64.data(), out.data(), kSamples, bits);
                consumeBuf(out.data(), out.size() * 4);
            }));
        }
        ae::TpdfQuantizer q;
        for (int bits : {16, 24, 32}) {
            char label[64];
            snprintf(label, sizeof(label), "ae::TpdfQuantizer  %d-bit target", bits);
            report(label, bench(kSamples, [&] {
                q.process(f64.data(), out.data(), kSamples, bits);
                consumeBuf(out.data(), out.size() * 4);
            }));
        }
        ae::NoiseShapedQuantizer nsq;
        for (int bits : {16, 24, 32}) {
            char label[64];
            snprintf(label, sizeof(label), "ae::NoiseShapedQuantizer  %d-bit, stereo", bits);
            report(label, bench(kSamples, [&] {
                nsq.process(f64.data(), out.data(), kSamples, bits, 2);
                consumeBuf(out.data(), out.size() * 4);
            }));
        }
    }

    // ---------------------------------------------------------- rounding ---
    printf("\nPer-sample rounding primitives:\n");
    {
        report("llround            (libm, current)", bench(kSamples, [&] {
            int64_t acc = 0;
            for (int i = 0; i < kSamples; ++i) acc += llround(f64[i] * 2147483647.0);
            consume(acc);
        }));
        report("ae::roundHalfAway  (inline, exact)", bench(kSamples, [&] {
            int64_t acc = 0;
            for (int i = 0; i < kSamples; ++i)
                acc += ae::roundHalfAway(f64[i] * 2147483647.0);
            consume(acc);
        }));
        report("lrint              (libm, current)", bench(kSamples, [&] {
            int64_t acc = 0;
            for (int i = 0; i < kSamples; ++i) acc += lrint(f64[i] * 2147483647.0);
            consume(acc);
        }));
        report("ae::roundHalfEven  (inline, exact)", bench(kSamples, [&] {
            int64_t acc = 0;
            for (int i = 0; i < kSamples; ++i)
                acc += ae::roundHalfEven(f64[i] * 2147483647.0);
            consume(acc);
        }));
    }

    // -------------------------------------------------------- RingBuffer ---
    printf("\nRingBuffer (byte throughput, reported per int32 sample):\n");
    {
        // 3 s at 44.1k/16-bit stereo, matching the driver's default sizing.
        ae::RingBuffer rb(44100 * 4 * 3);
        std::vector<uint8_t> src(kSamples * 4), dst(kSamples * 4);
        memcpy(src.data(), i32.data(), src.size());
        report("write + read round trip", bench(kSamples, [&] {
            rb.write(src.data(), src.size());
            rb.read(dst.data(), dst.size());
            consumeBuf(dst.data(), dst.size());
        }));
        report("submitTransfer-shaped read (32 x 176 B)", bench(kSamples, [&] {
            rb.write(src.data(), src.size());
            size_t got = 0;
            while (got < src.size()) {
                const size_t n = std::min((size_t)176, src.size() - got);
                got += rb.read(dst.data() + got, n);
            }
            consumeBuf(dst.data(), dst.size());
        }));
    }

    printf("\n(sink %llu)\n", (unsigned long long)g_sink);
    return 0;
}
