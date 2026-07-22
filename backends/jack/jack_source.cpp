#include "jack_source.h"

#include <jack/jack.h>

#include <cstdio>
#include <cstring>
#include <algorithm>

#define LOGI(...) do { fprintf(stderr, "[JackSource INFO] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define LOGE(...) do { fprintf(stderr, "[JackSource ERR] "  __VA_ARGS__); fputc('\n', stderr); } while (0)

JackSource::JackSource() = default;

JackSource::~JackSource() {
    close();
}

bool JackSource::open(const std::string& clientName) {
    if (client) close();
    jack_status_t status;
    // JackNoStartServer: connect to the user's running jackd only — never
    // auto-spawn one behind their back.
    client = jack_client_open(clientName.c_str(), JackNoStartServer, &status);
    if (!client) {
        LOGE("jack_client_open failed (status 0x%x) — is jackd running?", status);
        return false;
    }
    if (status & JackNameNotUnique) {
        LOGI("client name in use, server assigned: %s", jack_get_client_name(client));
    }
    LOGI("connected to JACK server: %u Hz, buffer %u frames",
         jack_get_sample_rate(client), jack_get_buffer_size(client));
    return true;
}

std::vector<JackCapturePortInfo> JackSource::enumerateCapturePorts() {
    std::vector<JackCapturePortInfo> out;
    if (!client) return out;
    // Hardware capture sources are OUTPUT ports from the graph's viewpoint
    // (they emit audio into the graph).
    const char** ports = jack_get_ports(client, nullptr, JACK_DEFAULT_AUDIO_TYPE,
                                        JackPortIsOutput | JackPortIsPhysical);
    if (!ports) return out;
    for (int i = 0; ports[i]; ++i) {
        out.push_back({ports[i]});
    }
    jack_free(ports);
    return out;
}

bool JackSource::configure(const ae::AudioFormat& fmt) {
    if (!client || streaming.load()) return false;
    if (fmt.channels < 1) return false;

    int serverRate = (int)jack_get_sample_rate(client);
    if (fmt.sampleRate > 0 && fmt.sampleRate != serverRate) {
        LOGI("rate hint %d ignored — the JACK server runs at %d Hz", fmt.sampleRate, serverRate);
    }
    if (fmt.bitDepth > 0 && fmt.bitDepth != 32) {
        LOGI("bit-depth hint %d ignored — JACK processes float32", fmt.bitDepth);
    }

    // Drop ports from a previous configure.
    for (jack_port_t* p : inputPorts) jack_port_unregister(client, p);
    inputPorts.clear();

    for (int i = 0; i < fmt.channels; ++i) {
        char name[32];
        snprintf(name, sizeof(name), "in_%d", i + 1);
        jack_port_t* p = jack_port_register(client, name, JACK_DEFAULT_AUDIO_TYPE,
                                            JackPortIsInput, 0);
        if (!p) {
            LOGE("jack_port_register(%s) failed", name);
            for (jack_port_t* q : inputPorts) jack_port_unregister(client, q);
            inputPorts.clear();
            return false;
        }
        inputPorts.push_back(p);
    }
    capChannels = fmt.channels;
    return true;
}

bool JackSource::start() {
    return start(std::vector<std::string>{});
}

bool JackSource::start(const std::vector<std::string>& sourcePorts) {
    if (!client || inputPorts.empty() || streaming.load()) return false;

    // 1 s float32 ring (same sizing philosophy as the other backends).
    size_t ringCapacity = (size_t)jack_get_sample_rate(client) * capChannels * sizeof(float);
    ring = new ae::RingBuffer(ringCapacity);
    overruns.store(0);

    // The callback interleaves into this scratch; sized for the server's
    // maximum buffer so the callback never allocates.
    interleaveBuf.resize((size_t)jack_get_buffer_size(client) * capChannels);

    // Callback must be set before activate.
    jack_set_process_callback(client, &JackSource::processTrampoline, this);

    streaming.store(true, std::memory_order_release);
    if (jack_activate(client) != 0) {
        LOGE("jack_activate failed");
        streaming.store(false, std::memory_order_release);
        delete ring;
        ring = nullptr;
        return false;
    }
    activated = true;

    // Wire our inputs: explicit ports if given, else first N physical ones.
    std::vector<std::string> sources = sourcePorts;
    if (sources.empty()) {
        for (const auto& p : enumerateCapturePorts()) {
            sources.push_back(p.portName);
            if ((int)sources.size() >= capChannels) break;
        }
    }
    if ((int)sources.size() < capChannels) {
        LOGE("only %zu capture sources available for %d channels", sources.size(), capChannels);
        stop();
        return false;
    }
    for (int i = 0; i < capChannels; ++i) {
        int err = jack_connect(client, sources[i].c_str(), jack_port_name(inputPorts[i]));
        if (err != 0 && err != EEXIST) {
            LOGE("jack_connect(%s -> %s) failed (%d)", sources[i].c_str(),
                 jack_port_name(inputPorts[i]), err);
            stop();
            return false;
        }
        LOGI("connected %s -> %s", sources[i].c_str(), jack_port_name(inputPorts[i]));
    }
    return true;
}

int JackSource::processTrampoline(uint32_t nframes, void* arg) {
    return static_cast<JackSource*>(arg)->process(nframes);
}

// REALTIME thread: no locks, no allocation, no I/O.
int JackSource::process(uint32_t nframes) {
    if (!streaming.load(std::memory_order_acquire) || !ring) return 0;

    const int ch = capChannels;
    float* dst = interleaveBuf.data();
    for (int c = 0; c < ch; ++c) {
        const float* src = (const float*)jack_port_get_buffer(inputPorts[c], nframes);
        for (uint32_t f = 0; f < nframes; ++f) {
            dst[f * ch + c] = src[f];
        }
    }

    size_t len = (size_t)nframes * ch * sizeof(float);
    size_t written = ring->write((const uint8_t*)dst, len);
    if (written < len) {
        overruns.fetch_add(1, std::memory_order_relaxed);   // consumer stalled
    }
    return 0;
}

int JackSource::read(uint8_t* out, int maxBytes) {
    if (!ring) return -1;
    if (!streaming.load(std::memory_order_acquire)) return -1;

    // Frame-align so consumers never see a torn float/frame.
    const size_t frameBytes = (size_t)capChannels * sizeof(float);
    size_t want = std::min((size_t)maxBytes, ring->getAvailable());
    want -= want % frameBytes;
    if (want == 0) return 0;
    return (int)ring->read(out, want);
}

void JackSource::stop() {
    if (!client) return;
    streaming.store(false, std::memory_order_release);
    if (activated) {
        // Deactivate detaches the process callback and disconnects our ports.
        jack_deactivate(client);
        activated = false;
    }
    int n = overruns.load();
    if (n > 0) LOGI("capture ended with %d ring overruns (consumer too slow)", n);
    delete ring;
    ring = nullptr;
}

void JackSource::close() {
    if (!client) return;
    stop();
    for (jack_port_t* p : inputPorts) jack_port_unregister(client, p);
    inputPorts.clear();
    jack_client_close(client);
    client = nullptr;
    capChannels = 0;
}

ae::AudioFormat JackSource::activeFormat() const {
    int rate = client ? (int)jack_get_sample_rate(client) : 0;
    return { rate, capChannels, 32, 4, true };
}
