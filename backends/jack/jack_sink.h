#ifndef AE_BACKENDS_JACK_SINK_H
#define AE_BACKENDS_JACK_SINK_H

// JACK2 playback backend (desktop Linux). Connects as a client to an
// already-running jackd using the real libjack — NEVER pipewire-jack. Registers
// output ports and, in the realtime process callback, drains a lock-free ring
// (fed by write()) into the port buffers, then connects to the server's
// physical playback ports.
//
// Implements ae::AudioSink: open -> configure -> start -> write -> stop ->
// close. JACK is float32 at the server's rate, so write() expects interleaved
// float32 (activeFormat().isFloat == true, subslotBytes == 4).

#include <cstdint>
#include <string>
#include <vector>
#include <atomic>

#include "core/audio_sink.h"
#include "core/buffer/ring_buffer.h"

typedef struct _jack_client jack_client_t;
typedef struct _jack_port jack_port_t;

struct JackPlaybackPortInfo {
    std::string portName;   // e.g. "system:playback_1"
};

class JackSink : public ae::AudioSink {
public:
    JackSink();
    ~JackSink() override;

    JackSink(const JackSink&) = delete;
    JackSink& operator=(const JackSink&) = delete;

    bool open(const std::string& clientName = "audio_engine_playback");

    // Physical playback sinks in the running server's graph. open() first.
    std::vector<JackPlaybackPortInfo> enumeratePlaybackPorts();

    // ae::AudioSink. fmt.channels registers that many output ports; the rate /
    // bit-depth are hints (the server dictates float32 at its own rate).
    bool configure(const ae::AudioFormat& fmt) override;
    // Activates the client and connects our outputs to the first N physical
    // playback ports.
    bool start() override;
    // Explicit variant: connect to the given destination ports (empty => first N).
    bool start(const std::vector<std::string>& destPorts);
    // Enqueue interleaved float32 for the RT callback. Returns bytes consumed
    // (< len when the ring is full), -1 if not started.
    int  write(const uint8_t* data, int len) override;
    void stop() override;
    ae::AudioFormat activeFormat() const override;   // {serverRate, ch, 32, 4, true}

    void close();

    bool isStreaming() const { return streaming.load(std::memory_order_acquire); }

private:
    static int processTrampoline(uint32_t nframes, void* arg);
    int process(uint32_t nframes);

    jack_client_t* client = nullptr;
    std::vector<jack_port_t*> outputPorts;
    int chans = 0;
    std::atomic<bool> streaming{false};
    bool activated = false;

    // Lock-free SPSC ring: writer = write() on a normal thread, reader = the
    // realtime process callback. Shared implementation, RT-safe.
    ae::RingBuffer* ring = nullptr;

    // Scratch the callback reads interleaved frames into before scattering them
    // to the per-channel port buffers (sized once in start(); no RT alloc).
    std::vector<float> scratch;

    std::atomic<int> underruns{0};
};

#endif // AE_BACKENDS_JACK_SINK_H
