// Smoke test: play a WAV file out a direct-hardware ALSA device (no server).
// Usage: play_wav_to_alsa <hw:N,D> <in.wav>
//   e.g. play_wav_to_alsa hw:0,0 sine.wav
// Integer PCM only (16/24/32-bit); the ALSA sink takes wire-format bytes so we
// stream the WAV data through unchanged once the device is configured to match.

#include <cstdio>
#include <vector>

#include "alsa_sink.h"
#include "wav_reader.h"

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <hw:N,D> <in.wav>\n", argv[0]);
        return 2;
    }
    const char* deviceId = argv[1];
    const char* inPath   = argv[2];

    WavReader wav;
    if (!wav.open(inPath)) {
        fprintf(stderr, "error: cannot open/parse WAV %s\n", inPath);
        return 1;
    }
    if (wav.isFloat()) {
        fprintf(stderr, "error: float WAV not supported by the ALSA sink (integer PCM only)\n");
        return 1;
    }
    printf("WAV: %d Hz, %d ch, %d-bit\n", wav.sampleRate(), wav.channels(), wav.bitDepth());

    AlsaSink sink;
    if (!sink.open(deviceId)) return 1;
    ae::AudioFormat fmt{};
    fmt.sampleRate   = wav.sampleRate();
    fmt.channels     = wav.channels();
    fmt.bitDepth     = wav.bitDepth();
    fmt.subslotBytes = wav.bitDepth() / 8;
    if (!sink.configure(fmt)) { fprintf(stderr, "error: sink.configure refused\n"); return 1; }
    if (!sink.start())        { fprintf(stderr, "error: sink.start failed\n"); return 1; }

    std::vector<uint8_t> buf(16384);
    for (;;) {
        int n = wav.read(buf.data(), (int)buf.size());
        if (n <= 0) break;
        int off = 0;
        while (off < n) {
            int w = sink.write(buf.data() + off, n - off);
            if (w < 0) { fprintf(stderr, "error: sink.write hard failure\n"); sink.close(); return 1; }
            off += w;
        }
    }

    sink.stop();
    sink.close();
    printf("done.\n");
    return 0;
}
