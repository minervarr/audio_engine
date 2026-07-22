#include "jack_sink.h"

#include <jack/jack.h>

#include <cstdio>

#define LOGI(...) do { fprintf(stderr, "[JackSink INFO] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define LOGE(...) do { fprintf(stderr, "[JackSink ERR] "  __VA_ARGS__); fputc('\n', stderr); } while (0)

JackSink::JackSink() = default;

JackSink::~JackSink() {
    close();
}

bool JackSink::open(const std::string& clientName) {
    if (client) close();
    jack_status_t status;
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

std::vector<JackPlaybackPortInfo> JackSink::enumeratePlaybackPorts() {
    std::vector<JackPlaybackPortInfo> out;
    if (!client) return out;
    // Physical playback sinks CONSUME audio, so they are INPUT ports from the
    // graph's viewpoint.
    const char** ports = jack_get_ports(client, nullptr, JACK_DEFAULT_AUDIO_TYPE,
                                        JackPortIsInput | JackPortIsPhysical);
    if (!ports) return out;
    for (int i = 0; ports[i]; ++i) {
        out.push_back({ports[i]});
    }
    jack_free(ports);
    return out;
}

bool JackSink::configure(const ae::AudioFormat& fmt) {
    if (!client || streaming.load()) return false;
    if (fmt.channels < 1) return false;

    int serverRate = (int)jack_get_sample_rate(client);
    if (fmt.sampleRate > 0 && fmt.sampleRate != serverRate) {
        LOGI("rate hint %d ignored — the JACK server runs at %d Hz", fmt.sampleRate, serverRate);
    }
    if (fmt.bitDepth > 0 && fmt.bitDepth != 32) {
        LOGI("bit-depth hint %d ignored — JACK processes float32", fmt.bitDepth);
    }

    for (jack_port_t* p : outputPorts) jack_port_unregister(client, p);
    outputPorts.clear();

    for (int i = 0; i < fmt.channels; ++i) {
        char name[32];
        snprintf(name, sizeof(name), "out_%d", i + 1);
        jack_port_t* p = jack_port_register(client, name, JACK_DEFAULT_AUDIO_TYPE,
                                            JackPortIsOutput, 0);
        if (!p) {
            LOGE("jack_port_register(%s) failed", name);
            for (jack_port_t* q : outputPorts) jack_port_unregister(client, q);
            outputPorts.clear();
            return false;
        }
        outputPorts.push_back(p);
    }
    chans = fmt.channels;
    return true;
}

bool JackSink::start() {
    return start(std::vector<std::string>{});
}

bool JackSink::start(const std::vector<std::string>& destPorts) {
    if (!client || outputPorts.empty() || streaming.load()) return false;

    // 1 s float32 ring (same sizing philosophy as the other backends).
    size_t ringCapacity = (size_t)jack_get_sample_rate(client) * chans * sizeof(float);
    ring = new ae::RingBuffer(ringCapacity);
    underruns.store(0);

    scratch.resize((size_t)jack_get_buffer_size(client) * chans);

    jack_set_process_callback(client, &JackSink::processTrampoline, this);

    streaming.store(true, std::memory_order_release);
    if (jack_activate(client) != 0) {
        LOGE("jack_activate failed");
        streaming.store(false, std::memory_order_release);
        delete ring;
        ring = nullptr;
        return false;
    }
    activated = true;

    // Wire our outputs to physical playback sinks: explicit ports if given,
    // else the first N.
    std::vector<std::string> dests = destPorts;
    if (dests.empty()) {
        for (const auto& p : enumeratePlaybackPorts()) {
            dests.push_back(p.portName);
            if ((int)dests.size() >= chans) break;
        }
    }
    if ((int)dests.size() < chans) {
        LOGE("only %zu playback sinks available for %d channels", dests.size(), chans);
        stop();
        return false;
    }
    for (int i = 0; i < chans; ++i) {
        int err = jack_connect(client, jack_port_name(outputPorts[i]), dests[i].c_str());
        if (err != 0 && err != EEXIST) {
            LOGE("jack_connect(%s -> %s) failed (%d)", jack_port_name(outputPorts[i]),
                 dests[i].c_str(), err);
            stop();
            return false;
        }
        LOGI("connected %s -> %s", jack_port_name(outputPorts[i]), dests[i].c_str());
    }
    return true;
}

int JackSink::processTrampoline(uint32_t nframes, void* arg) {
    return static_cast<JackSink*>(arg)->process(nframes);
}

// REALTIME thread: no locks, no allocation, no I/O.
int JackSink::process(uint32_t nframes) {
    const int ch = chans;

    // Pull up to nframes interleaved frames out of the ring in one shot.
    size_t wanted = (size_t)nframes * ch * sizeof(float);
    float* tmp = scratch.data();
    size_t got = (streaming.load(std::memory_order_acquire) && ring)
                     ? ring->read((uint8_t*)tmp, wanted) : 0;
    size_t gotFrames = got / ((size_t)ch * sizeof(float));

    for (int c = 0; c < ch; ++c) {
        float* pb = (float*)jack_port_get_buffer(outputPorts[c], nframes);
        for (uint32_t f = 0; f < nframes; ++f) {
            pb[f] = (f < gotFrames) ? tmp[f * ch + c] : 0.0f;   // zero-fill on underrun
        }
    }
    if (gotFrames < nframes) underruns.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

int JackSink::write(const uint8_t* data, int len) {
    if (!ring) return -1;
    if (!streaming.load(std::memory_order_acquire)) return -1;
    return (int)ring->write(data, (size_t)len);
}

void JackSink::stop() {
    if (!client) return;
    streaming.store(false, std::memory_order_release);
    if (activated) {
        jack_deactivate(client);
        activated = false;
    }
    int n = underruns.load();
    if (n > 0) LOGI("playback ended with %d ring underruns (producer too slow)", n);
    delete ring;
    ring = nullptr;
}

void JackSink::close() {
    if (!client) return;
    stop();
    for (jack_port_t* p : outputPorts) jack_port_unregister(client, p);
    outputPorts.clear();
    jack_client_close(client);
    client = nullptr;
    chans = 0;
}

ae::AudioFormat JackSink::activeFormat() const {
    int rate = client ? (int)jack_get_sample_rate(client) : 0;
    return { rate, chans, 32, 4, true };
}
