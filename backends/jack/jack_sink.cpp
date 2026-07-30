#include "jack_sink.h"

#include <jack/jack.h>

#include <cstdio>
#include <chrono>

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

    // Allocate the ring HERE, not in start(), so callers can pre-fill before the
    // client is activated. UsbAudioDriver::configure does the same for the same
    // reason (usb_audio.cpp:1157) — without it every chunk the decoder produces
    // between startAsyncInt32() and start() is rejected, and playback begins on
    // an empty ring that underruns instantly.
    const size_t frameBytes  = (size_t)chans * sizeof(float);
    size_t ringCap = (size_t)serverRate * frameBytes * kPlaybackRingMs / 1000;
    ringCap -= ringCap % frameBytes;              // whole frames (see write())
    delete ring;
    ring = new ae::RingBuffer(ringCap);
    underruns.store(0);
    xruns.store(0);
    faulted.store(false, std::memory_order_release);

    // Size the scatter scratch generously up front; the buffer-size callback
    // keeps it correct if the server's period changes later.
    serverPeriod.store((int)jack_get_buffer_size(client), std::memory_order_relaxed);
    scratch.resize((size_t)serverPeriod.load(std::memory_order_relaxed) * chans);

    acceptingWrites.store(true, std::memory_order_release);
    LOGI("ring allocated in configure(): %zu bytes (%d ms), period %d frames",
         ringCap, kPlaybackRingMs, serverPeriod.load(std::memory_order_relaxed));
    return true;
}

bool JackSink::start() {
    return start(std::vector<std::string>{});
}

bool JackSink::start(const std::vector<std::string>& destPorts) {
    if (!client || outputPorts.empty() || streaming.load()) return false;
    if (!ring) { LOGE("start() before configure() — no ring"); return false; }

    jack_set_process_callback(client, &JackSink::processTrampoline, this);
    // The server may change its period at any time. Without this callback
    // process() would read nframes*chans floats into a scratch sized for the
    // OLD period — a heap overflow, not merely a glitch.
    jack_set_buffer_size_callback(client, &JackSink::bufferSizeTrampoline, this);
    // Server-side deadline misses. Counted apart from our own ring underruns:
    // an xrun means jackd's period is too small for the machine, an underrun
    // means we starved it. Merging them hides which one is happening.
    jack_set_xrun_callback(client, &JackSink::xrunTrampoline, this);
    // Without this a server exit leaves the client zombied and playback simply
    // stops, with nothing in the app able to notice.
    jack_on_shutdown(client, &JackSink::shutdownTrampoline, this);

    streaming.store(true, std::memory_order_release);
    if (jack_activate(client) != 0) {
        LOGE("jack_activate failed");
        streaming.store(false, std::memory_order_release);
        return false;   // the ring belongs to configure(); leave it intact
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
    // Only now may process() start draining. Between jack_activate() above and
    // this point the callback is already running; had it been draining it would
    // have thrown away the pre-buffer into ports nothing was listening to.
    connected.store(true, std::memory_order_release);
    LOGI("streaming: pre-buffer %zu / %zu bytes (%.0f ms)",
         ringAvailable(), ringCapacity(),
         chans > 0 && jack_get_sample_rate(client) > 0
             ? ringAvailable() * 1000.0 / (chans * sizeof(float) * jack_get_sample_rate(client))
             : 0.0);
    return true;
}

int JackSink::processTrampoline(uint32_t nframes, void* arg) {
    return static_cast<JackSink*>(arg)->process(nframes);
}

// REALTIME thread: no locks, no allocation, no I/O — which is why underruns are
// only COUNTED here and reported from write() (a normal thread) instead.
int JackSink::process(uint32_t nframes) {
    const int ch = chans;
    if (ch <= 0) return 0;

    // Pull up to nframes interleaved frames out of the ring in one shot.
    size_t wanted = (size_t)nframes * ch * sizeof(float);
    // Belt and braces against a scratch sized for a smaller period: the
    // buffer-size callback should have resized it, but a read past the end here
    // would corrupt the heap from the RT thread — the worst place to debug.
    const size_t cap = scratch.size() * sizeof(float);
    if (wanted > cap) wanted = cap - (cap % ((size_t)ch * sizeof(float)));
    float* tmp = scratch.data();
    // Drain only once fully connected (see start()).
    size_t got = (connected.load(std::memory_order_acquire) && ring)
                     ? ring->read((uint8_t*)tmp, wanted) : 0;
    size_t gotFrames = got / ((size_t)ch * sizeof(float));

    // Deinterleave into JACK's per-port buffers. The `f < gotFrames` test used
    // to sit inside the per-sample loop; splitting it into a copy run and a
    // zero-fill tail drops a branch per sample on the RT thread and lets both
    // halves vectorise. Stereo gets its own pass so the interleaved read is
    // sequential rather than strided — it is the case that actually happens.
    if (ch == 2) {
        float* l = (float*)jack_port_get_buffer(outputPorts[0], nframes);
        float* r = (float*)jack_port_get_buffer(outputPorts[1], nframes);
        for (size_t f = 0; f < gotFrames; ++f) {
            l[f] = tmp[f * 2];
            r[f] = tmp[f * 2 + 1];
        }
        for (size_t f = gotFrames; f < nframes; ++f) { l[f] = 0.0f; r[f] = 0.0f; }
    } else {
        for (int c = 0; c < ch; ++c) {
            float* pb = (float*)jack_port_get_buffer(outputPorts[c], nframes);
            const float* src = tmp + c;
            for (size_t f = 0; f < gotFrames; ++f) pb[f] = src[f * ch];
            for (size_t f = gotFrames; f < nframes; ++f) pb[f] = 0.0f;  // underrun
        }
    }
    // Only a real underrun once we are connected — the deliberate silence
    // before that (see start()) is not a starvation event.
    if (gotFrames < nframes && connected.load(std::memory_order_relaxed))
        underruns.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

// Graph is stopped while this runs, so resizing is safe here (and only here).
int JackSink::bufferSizeTrampoline(uint32_t nframes, void* arg) {
    auto* self = static_cast<JackSink*>(arg);
    self->serverPeriod.store((int)nframes, std::memory_order_relaxed);
    if (self->chans > 0) self->scratch.resize((size_t)nframes * self->chans);
    LOGI("server period now %u frames", nframes);
    return 0;
}

int JackSink::xrunTrampoline(void* arg) {
    static_cast<JackSink*>(arg)->xruns.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

void JackSink::shutdownTrampoline(void* arg) {
    auto* self = static_cast<JackSink*>(arg);
    // The client is gone; the caller must stop rather than keep feeding it.
    self->connected.store(false, std::memory_order_release);
    self->acceptingWrites.store(false, std::memory_order_release);
    self->streaming.store(false, std::memory_order_release);
    self->faulted.store(true, std::memory_order_release);
    LOGE("JACK server shut down — playback cannot continue");
}

int JackSink::pendingFrames() const {
    if (!ring || chans <= 0) return 0;
    const size_t frameBytes = (size_t)chans * sizeof(float);
    int ringFrames = (int)(ring->getAvailable() / frameBytes);
    // One period is already in the port buffers, on its way to the device.
    return ringFrames + (connected.load(std::memory_order_acquire)
                             ? serverPeriod.load(std::memory_order_relaxed) : 0);
}

void JackSink::flush() {
    if (ring) ring->clear();
}

int JackSink::write(const uint8_t* data, int len) {
    if (!ring) return -1;
    // acceptingWrites, NOT streaming: writes are legal from configure() onward
    // so the caller can build a pre-buffer before start() activates the client.
    if (!acceptingWrites.load(std::memory_order_acquire)) return -1;
    if (len <= 0) return 0;

    reportUnderruns();

    // Whole frames only — this clamp is load-bearing, not hygiene.
    //
    // RingBuffer keeps one byte free to tell full from empty, so its free space
    // is never a multiple of the frame size. An unclamped write therefore parks
    // a PARTIAL frame in the ring whenever it fills. process() then reads a
    // non-frame-aligned byte count, renders floor(got/frameBytes) frames and
    // silently drops the trailing bytes — shifting the byte stream by 1..N-1
    // bytes for good. Every later frame reads across a frame boundary, so the
    // channel interleave is destroyed and the track becomes noise. Keeping the
    // ring frame-aligned makes process()'s read exact by construction.
    const size_t frameBytes = (size_t)chans * sizeof(float);
    if (frameBytes == 0) return -1;
    size_t room = ring->getFreeSpace();
    room -= room % frameBytes;
    const size_t n = std::min((size_t)len, room);
    if (n == 0) return 0;
    return (int)ring->write(data, n);
}

// Called from write() (a normal thread) because process() is realtime and must
// not do I/O. Rate-limited to ~1/s, matching the [USB][WARN] pattern in
// gui/src/audio_output.h — a glitch the user hears should leave a trace.
void JackSink::reportUnderruns() {
    const int n = underruns.load(std::memory_order_relaxed);
    if (n == lastReportedUnderruns_) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - lastUnderrunReport_ < std::chrono::seconds(1)) return;
    LOGI("%d ring underruns so far (+%d) — producer not keeping up; "
         "%d server xruns", n, n - lastReportedUnderruns_,
         xruns.load(std::memory_order_relaxed));
    lastReportedUnderruns_ = n;
    lastUnderrunReport_    = now;
}

void JackSink::stop() {
    if (!client) return;
    acceptingWrites.store(false, std::memory_order_release);
    connected.store(false, std::memory_order_release);
    streaming.store(false, std::memory_order_release);
    if (activated) {
        jack_deactivate(client);
        activated = false;
    }
    int n = underruns.load();
    if (n > 0) LOGI("playback ended with %d ring underruns (producer too slow), "
                    "%d server xruns", n, xruns.load());
    // The ring belongs to configure(), so only empty it — freeing here would
    // break configure -> start -> stop -> start, and would drop the pre-buffer
    // a subsequent start() is entitled to find.
    if (ring) ring->clear();
}

void JackSink::close() {
    if (!client) return;
    stop();
    delete ring;
    ring = nullptr;
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
