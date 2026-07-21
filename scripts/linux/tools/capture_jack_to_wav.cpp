// Smoke test: capture N seconds from the running JACK server to a WAV.
// Usage: capture_jack_to_wav <seconds> <out.wav> [channels]
// JACK delivers float32 at the server rate; we write S16 WAV for easy
// playback, converting with a plain truncating quantize (smoke test only).

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cstdint>
#include <vector>
#include <unistd.h>

#include "jack_capture.h"
#include "wav_writer.h"

int main(int argc, char** argv) {
    if (argc != 3 && argc != 4) {
        fprintf(stderr, "usage: %s <seconds> <out.wav> [channels=1]\n", argv[0]);
        return 2;
    }
    int seconds = atoi(argv[1]);
    const char* outPath = argv[2];
    int channels = (argc == 4) ? atoi(argv[3]) : 1;

    JackCaptureDriver driver;
    if (!driver.open("audio_engine_capture")) return 1;
    if (!driver.configureCapture(0, channels, 0)) { driver.close(); return 1; }
    if (!driver.startCapture()) { driver.close(); return 1; }

    int rate = driver.getConfiguredCaptureRate();
    printf("capturing %d s at %d Hz, %d ch (float32 -> S16 WAV)\n",
           seconds, rate, channels);

    WavWriter wav;
    if (!wav.open(outPath, rate, channels, 16)) {
        fprintf(stderr, "error: cannot open %s for writing\n", outPath);
        driver.close();
        return 1;
    }

    std::vector<uint8_t> raw(16384);
    std::vector<int16_t> pcm(raw.size() / sizeof(float));
    time_t end = time(nullptr) + seconds;
    while (time(nullptr) < end) {
        int n = driver.readCapture(raw.data(), (int)raw.size());
        if (n < 0) { fprintf(stderr, "error: readCapture hard failure\n"); break; }
        if (n == 0) { usleep(5 * 1000); continue; }

        int samples = n / (int)sizeof(float);
        const float* f = (const float*)raw.data();
        for (int i = 0; i < samples; ++i) {
            float v = f[i];
            if (v > 1.0f) v = 1.0f;
            if (v < -1.0f) v = -1.0f;
            pcm[i] = (int16_t)(v * 32767.0f);
        }
        wav.write((const uint8_t*)pcm.data(), (size_t)samples * sizeof(int16_t));
    }

    driver.close();
    wav.close();
    printf("wrote %s (%llu bytes of PCM)\n", outPath,
           (unsigned long long)wav.bytesWritten());
    return wav.bytesWritten() > 0 ? 0 : 1;
}
