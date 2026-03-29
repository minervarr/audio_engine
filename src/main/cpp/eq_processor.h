#ifndef EQ_PROCESSOR_H
#define EQ_PROCESSOR_H

#include <cstdint>
#include <cstring>
#include <cmath>

#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#define EQ_HAS_NEON 1
#else
#define EQ_HAS_NEON 0
#endif

class EqProcessor {
public:
    static constexpr int MAX_FILTERS = 10;

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
            for (int ch = 0; ch < 8; ch++) {
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

private:
    struct Biquad {
        double b0, b1, b2, a1, a2;
        double z1[8]; // per-channel delay line (transposed direct form II)
        double z2[8];
    };

    Biquad filters[MAX_FILTERS];
    int numFilters;
    float preampLinear;
    bool hasPreamp;
    int channelCount;
    int encoding;
    bool enabled;

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

#if EQ_HAS_NEON
    // NEON double-precision stereo biquad: processes L+R simultaneously
    // z1[0]=L, z1[1]=R are contiguous -- loads directly as float64x2_t
    inline void processStereoNeon(float64x2_t& xVec) {
        if (hasPreamp) {
            float64x2_t gain = vdupq_n_f64(static_cast<double>(preampLinear));
            xVec = vmulq_f64(xVec, gain);
        }
        for (int f = 0; f < numFilters; f++) {
            Biquad& bq = filters[f];
            float64x2_t b0 = vdupq_n_f64(bq.b0);
            float64x2_t b1 = vdupq_n_f64(bq.b1);
            float64x2_t b2 = vdupq_n_f64(bq.b2);
            float64x2_t a1 = vdupq_n_f64(bq.a1);
            float64x2_t a2 = vdupq_n_f64(bq.a2);
            float64x2_t z1 = vld1q_f64(&bq.z1[0]);
            float64x2_t z2 = vld1q_f64(&bq.z2[0]);

            // y = b0*x + z1
            float64x2_t y = vfmaq_f64(z1, b0, xVec);
            // z1 = b1*x - a1*y + z2
            float64x2_t newZ1 = vfmaq_f64(z2, b1, xVec);
            newZ1 = vfmsq_f64(newZ1, a1, y);
            // z2 = b2*x - a2*y
            float64x2_t newZ2 = vfmsq_f64(vmulq_f64(b2, xVec), a2, y);

            vst1q_f64(&bq.z1[0], newZ1);
            vst1q_f64(&bq.z2[0], newZ2);
            xVec = y;
        }
    }

    void processStereoFloat_NEON(float* samples, int count) {
        for (int i = 0; i < count; i += 2) {
            // Load L+R as float, widen to double
            float32x2_t sf = vld1_f32(&samples[i]);
            float64x2_t xVec = vcvt_f64_f32(sf);
            processStereoNeon(xVec);
            // Narrow back to float, store
            float32x2_t result = vcvt_f32_f64(xVec);
            vst1_f32(&samples[i], result);
        }
    }

    void processStereo16_NEON(int16_t* samples, int count) {
        const float64x2_t scale = vdupq_n_f64(1.0 / 32768.0);
        const float64x2_t scaleOut = vdupq_n_f64(32767.0);
        const float64x2_t posOne = vdupq_n_f64(1.0);
        const float64x2_t negOne = vdupq_n_f64(-1.0);
        for (int i = 0; i < count; i += 2) {
            // Load two int16 samples, widen to double
            double dL = samples[i] / 32768.0;
            double dR = samples[i + 1] / 32768.0;
            float64x2_t xVec = {dL, dR};
            processStereoNeon(xVec);
            // Clamp to [-1, 1]
            xVec = vminq_f64(xVec, posOne);
            xVec = vmaxq_f64(xVec, negOne);
            xVec = vmulq_f64(xVec, scaleOut);
            samples[i]     = static_cast<int16_t>(vgetq_lane_f64(xVec, 0));
            samples[i + 1] = static_cast<int16_t>(vgetq_lane_f64(xVec, 1));
        }
    }

    void processStereo32_NEON(int32_t* samples, int count) {
        const float64x2_t scaleOut = vdupq_n_f64(2147483647.0);
        const float64x2_t posOne = vdupq_n_f64(1.0);
        const float64x2_t negOne = vdupq_n_f64(-1.0);
        for (int i = 0; i < count; i += 2) {
            double dL = samples[i] / 2147483648.0;
            double dR = samples[i + 1] / 2147483648.0;
            float64x2_t xVec = {dL, dR};
            processStereoNeon(xVec);
            xVec = vminq_f64(xVec, posOne);
            xVec = vmaxq_f64(xVec, negOne);
            xVec = vmulq_f64(xVec, scaleOut);
            samples[i]     = static_cast<int32_t>(vgetq_lane_f64(xVec, 0));
            samples[i + 1] = static_cast<int32_t>(vgetq_lane_f64(xVec, 1));
        }
    }
#endif // EQ_HAS_NEON

    void processFloat(float* samples, int count) {
#if EQ_HAS_NEON
        if (channelCount == 2) {
            processStereoFloat_NEON(samples, count);
            return;
        }
#else
        if (channelCount == 2) {
            for (int i = 0; i < count; i += 2) {
                samples[i]     = static_cast<float>(processSample(samples[i],     0));
                samples[i + 1] = static_cast<float>(processSample(samples[i + 1], 1));
            }
            return;
        }
#endif
        int frames = count / channelCount;
        for (int f = 0; f < frames; f++) {
            int base = f * channelCount;
            for (int ch = 0; ch < channelCount; ch++) {
                samples[base + ch] = static_cast<float>(
                    processSample(samples[base + ch], ch));
            }
        }
    }

    void process16(int16_t* samples, int count) {
#if EQ_HAS_NEON
        if (channelCount == 2) {
            processStereo16_NEON(samples, count);
            return;
        }
#else
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
#endif
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
                // Little-endian 24-bit signed
                int32_t val = data[off] | (data[off + 1] << 8) | (data[off + 2] << 16);
                if (val & 0x800000) val |= 0xFF000000; // sign extend

                double s = val / 8388608.0;
                s = processSample(s, ch);
                if (s > 1.0) s = 1.0;
                else if (s < -1.0) s = -1.0;
                int32_t out = static_cast<int32_t>(s * 8388607.0);
                data[off]     = out & 0xFF;
                data[off + 1] = (out >> 8) & 0xFF;
                data[off + 2] = (out >> 16) & 0xFF;
            }
        }
    }

    void process32(int32_t* samples, int count) {
#if EQ_HAS_NEON
        if (channelCount == 2) {
            processStereo32_NEON(samples, count);
            return;
        }
#else
        if (channelCount == 2) {
            for (int i = 0; i < count; i += 2) {
                double s0 = samples[i] / 2147483648.0;
                s0 = processSample(s0, 0);
                if (s0 > 1.0) s0 = 1.0; else if (s0 < -1.0) s0 = -1.0;
                samples[i] = static_cast<int32_t>(s0 * 2147483647.0);

                double s1 = samples[i + 1] / 2147483648.0;
                s1 = processSample(s1, 1);
                if (s1 > 1.0) s1 = 1.0; else if (s1 < -1.0) s1 = -1.0;
                samples[i + 1] = static_cast<int32_t>(s1 * 2147483647.0);
            }
            return;
        }
#endif
        int frames = count / channelCount;
        for (int f = 0; f < frames; f++) {
            int base = f * channelCount;
            for (int ch = 0; ch < channelCount; ch++) {
                double s = samples[base + ch] / 2147483648.0;
                s = processSample(s, ch);
                if (s > 1.0) s = 1.0; else if (s < -1.0) s = -1.0;
                samples[base + ch] = static_cast<int32_t>(s * 2147483647.0);
            }
        }
    }
};

#endif // EQ_PROCESSOR_H
