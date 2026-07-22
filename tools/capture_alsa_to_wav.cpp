// Smoke test: capture N seconds from an ALSA device to a WAV file.
// Usage: capture_alsa_to_wav <deviceId> <seconds> <out.wav>
//   e.g. capture_alsa_to_wav hw:1,0 5 test.wav
//        capture_alsa_to_wav default 5 test.wav

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <vector>

#include "alsa_source.h"
#include "wav_writer.h"

int main(int argc, char** argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <deviceId> <seconds> <out.wav>\n", argv[0]);
        return 2;
    }
    const char* deviceId = argv[1];
    int seconds = atoi(argv[2]);
    const char* outPath = argv[3];

    AlsaSource driver;
    if (!driver.open(deviceId)) return 1;

    // 48 kHz stereo 16-bit request; the driver negotiates what the hardware
    // actually supports and reports the effective values.
    if (!driver.configure({48000, 2, 16, 0, false})) {
        driver.close();
        return 1;
    }
    if (!driver.start()) {
        driver.close();
        return 1;
    }

    const ae::AudioFormat fmt = driver.activeFormat();
    WavWriter wav;
    if (!wav.open(outPath, fmt.sampleRate, fmt.channels, fmt.subslotBytes * 8)) {
        fprintf(stderr, "error: cannot open %s for writing\n", outPath);
        driver.close();
        return 1;
    }

    std::vector<uint8_t> buf(8192);
    time_t end = time(nullptr) + seconds;
    while (time(nullptr) < end) {
        int n = driver.read(buf.data(), (int)buf.size());
        if (n < 0) { fprintf(stderr, "error: readCapture hard failure\n"); break; }
        if (n == 0) continue;   // ring read already waits up to ~100 ms
        wav.write(buf.data(), (size_t)n);
    }

    driver.close();
    wav.close();
    printf("wrote %s (%llu bytes of PCM)\n", outPath,
           (unsigned long long)wav.bytesWritten());
    return wav.bytesWritten() > 0 ? 0 : 1;
}
