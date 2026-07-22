// Smoke test: play a WAV file into the running JACK server (physical playback
// ports). Usage: play_wav_to_jack <in.wav>
// JACK is float32 at the server's rate, so we convert the WAV's integer PCM to
// interleaved float32 and feed the sink. No resampling: if the WAV rate differs
// from the server rate the pitch will be off — fine for a smoke test.

#include <cstdio>
#include <cstdint>
#include <vector>
#include <unistd.h>

#include "jack_sink.h"
#include "wav_reader.h"

// Convert one interleaved integer-PCM buffer to float32 in [-1, 1].
static void pcmToFloat(const uint8_t* in, int inBytes, int bits, std::vector<float>& out) {
    if (bits == 16) {
        int n = inBytes / 2;
        out.resize(n);
        const int16_t* s = (const int16_t*)in;
        for (int i = 0; i < n; ++i) out[i] = s[i] / 32768.0f;
    } else if (bits == 24) {
        int n = inBytes / 3;
        out.resize(n);
        for (int i = 0; i < n; ++i) {
            int32_t v = (in[i*3] << 8) | (in[i*3+1] << 16) | (in[i*3+2] << 24);
            v >>= 8;  // arithmetic shift keeps the sign
            out[i] = v / 8388608.0f;
        }
    } else { // 32-bit
        int n = inBytes / 4;
        out.resize(n);
        const int32_t* s = (const int32_t*)in;
        for (int i = 0; i < n; ++i) out[i] = s[i] / 2147483648.0f;
    }
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <in.wav>\n", argv[0]);
        return 2;
    }
    const char* inPath = argv[1];

    WavReader wav;
    if (!wav.open(inPath)) {
        fprintf(stderr, "error: cannot open/parse WAV %s\n", inPath);
        return 1;
    }
    if (wav.isFloat()) {
        fprintf(stderr, "error: this smoke tool expects integer PCM input\n");
        return 1;
    }
    printf("WAV: %d Hz, %d ch, %d-bit\n", wav.sampleRate(), wav.channels(), wav.bitDepth());

    JackSink sink;
    if (!sink.open("audio_engine_play")) return 1;
    if (wav.sampleRate() != sink.activeFormat().sampleRate) {
        fprintf(stderr, "warning: WAV %d Hz != JACK server %d Hz (no resample; pitch will shift)\n",
                wav.sampleRate(), sink.activeFormat().sampleRate);
    }
    ae::AudioFormat fmt{};
    fmt.channels = wav.channels();
    if (!sink.configure(fmt)) { fprintf(stderr, "error: sink.configure refused\n"); sink.close(); return 1; }
    if (!sink.start())        { fprintf(stderr, "error: sink.start failed\n"); sink.close(); return 1; }

    std::vector<uint8_t> raw(16384);
    std::vector<float> flt;
    for (;;) {
        int n = wav.read(raw.data(), (int)raw.size());
        if (n <= 0) break;
        pcmToFloat(raw.data(), n, wav.bitDepth(), flt);
        const uint8_t* bytes = (const uint8_t*)flt.data();
        int total = (int)(flt.size() * sizeof(float));
        int off = 0;
        while (off < total) {
            int w = sink.write(bytes + off, total - off);
            if (w < 0) { fprintf(stderr, "error: sink.write hard failure\n"); sink.close(); return 1; }
            if (w == 0) { usleep(2 * 1000); continue; }   // ring full — let the RT thread drain
            off += w;
        }
    }

    // Let the ring drain before tearing down (rough: one buffer's worth).
    usleep(200 * 1000);
    sink.stop();
    sink.close();
    printf("done.\n");
    return 0;
}
