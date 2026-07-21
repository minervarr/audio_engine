#ifndef JACK_CAPTURE_H
#define JACK_CAPTURE_H

// JACK2 capture backend (desktop Linux). Connects as a client to an
// already-running jackd (the user starts it, e.g. via qjackctl) using the
// real libjack — NEVER the pipewire-jack shim (verify with:
//   ldd capture_jack_to_wav | grep jack   -> must NOT mention pipewire).
//
// Mirrors UsbAudioDriver's capture surface: open -> configureCapture ->
// startCapture -> poll readCapture -> stopCapture -> close.
//
// JACK owns the sample rate and processes float32 — configureCapture's rate
// and bit-depth arguments are treated as hints and warned about when they
// don't match; readCapture delivers interleaved float32 frames at the
// server's rate (subslot size 4).

#include <cstdint>
#include <string>
#include <vector>
#include <atomic>

typedef struct _jack_client jack_client_t;
typedef struct _jack_port jack_port_t;

struct JackCapturePortInfo {
    std::string portName;   // e.g. "system:capture_1"
};

class JackCaptureDriver {
public:
    JackCaptureDriver();
    ~JackCaptureDriver();

    JackCaptureDriver(const JackCaptureDriver&) = delete;
    JackCaptureDriver& operator=(const JackCaptureDriver&) = delete;

    // Opens a client connection to the running server. Fails (false) when
    // no jackd is running — the engine never starts a server itself.
    bool open(const std::string& clientName = "audio_engine_capture");

    // Physical hardware capture sources visible in the running server's
    // graph. Requires open() first.
    std::vector<JackCapturePortInfo> enumerateCapturePorts();

    // Registers `channels` input ports. rate/bitDepth are hints only (the
    // server dictates float32 at its own rate); mismatches are logged, not
    // errors — matching the "ignored (logged)" precedent in usb_audio.h.
    bool configureCapture(int sampleRateHint, int channels, int bitDepthHint);

    // Activates the client and connects our inputs to `sourcePorts`
    // (empty => the first N physical capture ports).
    bool startCapture(const std::vector<std::string>& sourcePorts = {});

    // Non-blocking drain of the lock-free ring fed by the process callback.
    // Returns -1 when not capturing, 0 when nothing buffered, else bytes
    // copied (interleaved float32, frame-aligned).
    int readCapture(uint8_t* out, int maxBytes);

    void stopCapture();
    void close();

    int  getConfiguredCaptureRate() const;                       // server rate
    int  getConfiguredCaptureChannels() const { return capChannels; }
    int  getConfiguredCaptureBitDepth() const { return 32; }     // float32
    int  getConfiguredCaptureSubslotSize() const { return 4; }
    bool isCapturing() const { return streaming.load(std::memory_order_acquire); }

private:
    static int processTrampoline(uint32_t nframes, void* arg);
    int process(uint32_t nframes);

    jack_client_t* client = nullptr;
    std::vector<jack_port_t*> inputPorts;
    int capChannels = 0;
    std::atomic<bool> streaming{false};
    bool activated = false;

    // Lock-free SPSC byte ring. Writer = JACK's realtime process callback,
    // reader = readCapture() on a normal thread. Same atomic readPos/writePos
    // pattern as usb_audio.h's RingBuffer — deliberately NOT the mutex+CV
    // NativePcmBuffer, which must never be touched from a JACK callback.
    uint8_t* ring = nullptr;
    size_t ringCapacity = 0;
    std::atomic<size_t> ringRead{0};
    std::atomic<size_t> ringWrite{0};

    // Scratch for interleaving inside the callback (sized once in
    // startCapture; the callback itself never allocates).
    std::vector<float> interleaveBuf;

    std::atomic<int> overruns{0};
};

#endif // JACK_CAPTURE_H
