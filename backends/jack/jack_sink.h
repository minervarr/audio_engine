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
#include <chrono>

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
    // Also allocates the ring, so callers can pre-fill before start() — the
    // same contract as UsbAudioDriver::configure (usb_audio.cpp:1157).
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

    // Discard everything queued but not yet rendered. For seeking: without it
    // the ring keeps playing up to kPlaybackRingMs of pre-seek audio.
    void flush();

    // Buffer telemetry, mirroring UsbAudioDriver's (usb_audio.h:110-116) so the
    // app's pre-buffer / position / drain logic works identically on both.
    size_t ringAvailable() const { return ring ? ring->getAvailable() : 0; }
    size_t ringCapacity()  const { return ring ? ring->getCapacity()  : 0; }
    // Frames accepted but not yet heard: ring occupancy plus the one server
    // period already handed to the port buffers. The JACK analogue of USB's
    // in-flight isochronous accounting (usb_audio.cpp:2087-2098).
    int pendingFrames() const;

    // True once the server has shut down or the client was zombified. The app
    // polls this (PlayerWindow::onTimer) to stop cleanly instead of feeding a
    // dead client forever.
    bool hasFaulted() const { return faulted.load(std::memory_order_acquire); }

    // Ring underruns (we starved the callback) vs server xruns (the server
    // missed its deadline). Different causes, opposite fixes — never merge them.
    int underrunCount() const { return underruns.load(std::memory_order_relaxed); }
    int xrunCount()     const { return xruns.load(std::memory_order_relaxed); }

private:
    static int processTrampoline(uint32_t nframes, void* arg);
    int process(uint32_t nframes);
    // JACK2 runs these with the graph stopped (buffer size) or off the RT
    // thread (xrun/shutdown), so unlike process() they may allocate and log.
    static int  bufferSizeTrampoline(uint32_t nframes, void* arg);
    static int  xrunTrampoline(void* arg);
    static void shutdownTrampoline(void* arg);
    // Logs accumulated underruns from write()'s thread; process() may not.
    void reportUnderruns();

    // Matches UsbAudioDriver::playbackRingMs (usb_audio.h:235). The old 1 s ring
    // could not absorb a single slow decode chunk (disk read, soxr VHQ pass)
    // before the callback ran dry and zero-filled — audible as a brief stop.
    static constexpr int kPlaybackRingMs = 3000;

    jack_client_t* client = nullptr;
    std::vector<jack_port_t*> outputPorts;
    int chans = 0;
    std::atomic<bool> streaming{false};
    bool activated = false;

    // write() is accepted from configure() until stop(), NOT merely while
    // streaming — that window is precisely where the pre-buffer gets built.
    std::atomic<bool> acceptingWrites{false};
    // process() renders silence until every port is connected. Draining earlier
    // would consume the pre-buffer into ports nobody is listening to yet.
    std::atomic<bool> connected{false};
    std::atomic<bool> faulted{false};
    std::atomic<int>  xruns{0};
    // Cached jack_get_buffer_size(); refreshed by the buffer-size callback.
    std::atomic<int>  serverPeriod{0};

    // Lock-free SPSC ring: writer = write() on a normal thread, reader = the
    // realtime process callback. Shared implementation, RT-safe.
    ae::RingBuffer* ring = nullptr;

    // Scratch the callback reads interleaved frames into before scattering them
    // to the per-channel port buffers (sized once in start(); no RT alloc).
    std::vector<float> scratch;

    std::atomic<int> underruns{0};
    // Touched only by write()'s thread — no synchronisation needed.
    int lastReportedUnderruns_ = 0;
    std::chrono::steady_clock::time_point lastUnderrunReport_{};
};

#endif // AE_BACKENDS_JACK_SINK_H
