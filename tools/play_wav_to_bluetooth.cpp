// Smoke test: play a WAV file to a pair of Bluetooth headphones, encoding SBC
// ourselves and writing straight to the AVDTP socket -- no PipeWire, no
// PulseAudio, nothing between the encoder and the radio.
//
//     play_wav_to_bluetooth <MAC|substring of the name> <in.wav>
//     play_wav_to_bluetooth 54:15:89:22:C5:D9 sine44.wav
//     play_wav_to_bluetooth LG-PL7 sine44.wav
//
// 16-bit integer WAV only -- SBC's input is S16 and this tool deliberately does
// not resample or requantise, so what it proves is the transport rather than a
// conversion this engine already has tested elsewhere.
//
// --- before it can work -----------------------------------------------------
//
// One A2DP transport per device, one owner. On an ordinary desktop PipeWire
// takes the headphones the moment they connect, and then this tool cannot have
// them. Hand them over first:
//
//     pactl set-card-profile bluez_card.54_15_89_22_C5_D9 off
//
// and give them back the same way (profile a2dp-sink) when finished. If that is
// skipped the tool says so in words rather than playing to nothing -- which is
// the point of the message, and worth checking it still appears.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "backends/bluetooth/bluetooth_sink.h"
#include "backends/bluetooth/bluez_a2dp.h"
#include "wav_reader.h"

using namespace ae;

static bool matches(const bluez::SinkDevice& d, const std::string& want) {
    if (want.empty()) return false;
    if (strcasecmp(d.address.c_str(), want.c_str()) == 0) return true;
    // A substring of the name, so "LG-PL7" finds "LG-PL7(D9)".
    std::string name = d.name, needle = want;
    for (char& c : name)   c = (char)tolower((unsigned char)c);
    for (char& c : needle) c = (char)tolower((unsigned char)c);
    return name.find(needle) != std::string::npos;
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <MAC|name> <in.wav>\n", argv[0]);
        return 2;
    }
    const std::string want = argv[1];
    const char* inPath     = argv[2];

    WavReader wav;
    if (!wav.open(inPath)) {
        fprintf(stderr, "error: cannot open/parse WAV %s\n", inPath);
        return 1;
    }
    if (wav.isFloat() || wav.bitDepth() != 16) {
        fprintf(stderr, "error: SBC takes 16-bit integer PCM; this WAV is %d-bit%s\n",
                wav.bitDepth(), wav.isFloat() ? " float" : "");
        return 1;
    }
    printf("WAV: %d Hz, %d ch, %d-bit\n", wav.sampleRate(), wav.channels(),
           wav.bitDepth());

    bluez::Bus bus;
    if (!bus.open()) {
        fprintf(stderr, "error: BlueZ unreachable: %s\n", bus.lastError().c_str());
        return 1;
    }

    bluez::SinkDevice dev;
    bool found = false;
    for (const bluez::SinkDevice& d : bus.sinks()) {
        if (!matches(d, want)) continue;
        dev = d;
        found = true;
        break;
    }
    if (!found) {
        fprintf(stderr, "error: no A2DP device matching \"%s\". "
                        "Run list_bluetooth_sinks to see what BlueZ knows.\n",
                want.c_str());
        return 1;
    }
    if (!dev.connected) {
        fprintf(stderr, "error: %s [%s] is paired but not connected.\n",
                dev.name.c_str(), dev.address.c_str());
        return 1;
    }
    printf("device: %s [%s]\n", dev.name.c_str(), dev.address.c_str());

    // Named before the attempt, because it is by far the most likely reason the
    // next step fails and the message is easier to act on beforehand.
    const bluez::TransportState t = bus.transportFor(dev.path);
    if (t.exists)
        printf("note  : %s already carries a transport (%s) -- something else "
               "owns this device.\n", dev.name.c_str(), t.state.c_str());

    BluetoothSink sink;
    if (!sink.open(dev)) {
        fprintf(stderr, "error: %s\n", sink.lastError().c_str());
        return 1;
    }

    AudioFormat fmt{};
    fmt.sampleRate   = wav.sampleRate();
    fmt.channels     = wav.channels();
    fmt.bitDepth     = 16;
    fmt.subslotBytes = 2;
    if (!sink.configure(fmt)) {
        fprintf(stderr, "error: %s\n", sink.lastError().c_str());
        return 1;
    }
    if (!sink.start()) {
        fprintf(stderr, "error: %s\n", sink.lastError().c_str());
        return 1;
    }

    const AudioFormat active = sink.activeFormat();
    printf("stream: SBC %d Hz, %d ch, bitpool %d\n", active.sampleRate,
           active.channels, sink.bitpool());

    std::vector<uint8_t> buf(16384);
    for (;;) {
        const int n = wav.read(buf.data(), (int)buf.size());
        if (n <= 0) break;
        int off = 0;
        while (off < n) {
            const int w = sink.write(buf.data() + off, n - off);
            if (w < 0) {
                fprintf(stderr, "error: %s\n", sink.lastError().c_str());
                sink.close();
                return 1;
            }
            off += w;
        }
    }

    sink.stop();
    sink.close();
    printf("done.\n");
    return 0;
}
