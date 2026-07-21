// Smoke test: capture N seconds from a USB Audio Class device (ADC/mic
// side) straight over libusb, write a WAV. Usage:
//   capture_usb_to_wav <vid_hex> <pid_hex> <seconds> <out.wav>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>
#include <unistd.h>

#include "usb_audio.h"
#include "wav_writer.h"

int main(int argc, char** argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: %s <vid_hex> <pid_hex> <seconds> <out.wav>\n", argv[0]);
        return 2;
    }
    uint16_t vid = (uint16_t)strtol(argv[1], nullptr, 16);
    uint16_t pid = (uint16_t)strtol(argv[2], nullptr, 16);
    int seconds = atoi(argv[3]);
    const char* outPath = argv[4];

    UsbAudioDriver driver;
    if (!driver.open(vid, pid)) {
        fprintf(stderr, "error: cannot open USB device %04x:%04x (permissions? udev rule?)\n", vid, pid);
        return 1;
    }
    if (!driver.parseDescriptors()) {
        fprintf(stderr, "error: failed to parse UAC descriptors\n");
        return 1;
    }

    auto tuples = driver.getCaptureFormatTuples();
    if (tuples.empty()) {
        fprintf(stderr, "error: device reports no capture formats\n");
        return 1;
    }
    // First tuple as default; prefer 48000/2/16 or 16000/1/16 when offered.
    int rate = tuples[0], ch = tuples[1], bits = tuples[2];
    for (size_t i = 0; i + 2 < tuples.size(); i += 3) {
        if ((tuples[i] == 48000 && tuples[i+2] == 16) ||
            (tuples[i] == 16000 && tuples[i+1] == 1 && tuples[i+2] == 16)) {
            rate = tuples[i]; ch = tuples[i+1]; bits = tuples[i+2];
            break;
        }
    }
    printf("capture format: %d Hz, %d ch, %d-bit\n", rate, ch, bits);

    if (!driver.configureCapture(rate, ch, bits)) {
        fprintf(stderr, "error: configureCapture(%d,%d,%d) refused\n", rate, ch, bits);
        return 1;
    }
    if (!driver.startCapture()) {
        fprintf(stderr, "error: startCapture failed\n");
        return 1;
    }

    // The wire subslot may be wider than the significant bits; ask the driver.
    int wireBits = driver.getConfiguredCaptureSubslotSize() * 8;
    WavWriter wav;
    if (!wav.open(outPath, driver.getConfiguredCaptureRate(),
                  driver.getConfiguredCaptureChannels(), wireBits)) {
        fprintf(stderr, "error: cannot open %s for writing\n", outPath);
        driver.stopCapture();
        driver.close();
        return 1;
    }

    std::vector<uint8_t> buf(8192);
    time_t end = time(nullptr) + seconds;
    while (time(nullptr) < end) {
        int n = driver.readCapture(buf.data(), (int)buf.size());
        if (n < 0) {
            fprintf(stderr, "error: readCapture hard failure\n");
            break;
        }
        if (n == 0) { usleep(10 * 1000); continue; }
        wav.write(buf.data(), (size_t)n);
    }

    driver.stopCapture();
    driver.close();
    wav.close();
    printf("wrote %s (%llu bytes of PCM)\n", outPath,
           (unsigned long long)wav.bytesWritten());
    return wav.bytesWritten() > 0 ? 0 : 1;
}
