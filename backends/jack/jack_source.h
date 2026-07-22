#ifndef AE_BACKENDS_JACK_SOURCE_H
#define AE_BACKENDS_JACK_SOURCE_H

// JACK2 capture backend (desktop Linux). Connects as a client to an
// already-running jackd (the user starts it, e.g. via qjackctl) using the
// real libjack — NEVER the pipewire-jack shim (verify with:
//   ldd capture_jack_to_wav | grep jack   -> must NOT mention pipewire).
//
// Implements ae::AudioSource: open -> configure -> start -> poll read -> stop
// -> close. JACK owns the sample rate and processes float32 — configure()'s
// rate and bit-depth are treated as hints and warned about when they don't
// match; read() delivers interleaved float32 frames at the server's rate
// (subslot size 4, isFloat=true).

#include <cstdint>
#include <string>
#include <vector>
#include <atomic>

#include "core/audio_source.h"
#include "core/buffer/ring_buffer.h"

typedef struct _jack_client jack_client_t;
typedef struct _jack_port jack_port_t;

struct JackCapturePortInfo {
    std::string portName;   // e.g. "system:capture_1"
};

class JackSource : public ae::AudioSource {
public:
    JackSource();
    ~JackSource() override;

    JackSource(const JackSource&) = delete;
    JackSource& operator=(const JackSource&) = delete;

    // Opens a client connection to the running server. Fails (false) when no
    // jackd is running — the engine never starts a server itself.
    bool open(const std::string& clientName = "audio_engine_capture");

    // Physical hardware capture sources in the running server's graph. open() first.
    std::vector<JackCapturePortInfo> enumerateCapturePorts();

    // ae::AudioSource. fmt.channels registers that many input ports; the rate /
    // bit-depth are hints only (the server dictates float32 at its own rate).
    bool configure(const ae::AudioFormat& fmt) override;
    // Activates the client and connects our inputs to the first N physical
    // capture ports.
    bool start() override;
    // Explicit variant: connect to the given source ports (empty => first N).
    bool start(const std::vector<std::string>& sourcePorts);
    // Non-blocking drain of the lock-free ring fed by the process callback.
    // Returns -1 when not capturing, 0 when nothing buffered, else bytes copied
    // (interleaved float32, frame-aligned).
    int  read(uint8_t* out, int maxBytes) override;
    void stop() override;
    ae::AudioFormat activeFormat() const override;   // {serverRate, ch, 32, 4, true}

    void close();

    bool isCapturing() const { return streaming.load(std::memory_order_acquire); }

private:
    static int processTrampoline(uint32_t nframes, void* arg);
    int process(uint32_t nframes);

    jack_client_t* client = nullptr;
    std::vector<jack_port_t*> inputPorts;
    int capChannels = 0;
    std::atomic<bool> streaming{false};
    bool activated = false;

    // Lock-free SPSC byte ring shared with the other RT backends. Writer =
    // JACK's realtime process callback, reader = read() on a normal thread.
    // Deliberately NOT the mutex+CV NativePcmBuffer, which must never be
    // touched from a JACK callback.
    ae::RingBuffer* ring = nullptr;

    // Scratch for interleaving inside the callback (sized once in start(); the
    // callback itself never allocates).
    std::vector<float> interleaveBuf;

    std::atomic<int> overruns{0};
};

#endif // AE_BACKENDS_JACK_SOURCE_H
