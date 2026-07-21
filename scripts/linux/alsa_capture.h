#ifndef ALSA_CAPTURE_H
#define ALSA_CAPTURE_H

// Direct-hardware ALSA capture backend (desktop Linux). No sound server
// involved — talks straight to the kernel driver, Audacity-style.
// Mirrors UsbAudioDriver's capture surface so consumers can treat every
// backend uniformly: open -> configureCapture -> startCapture ->
// poll readCapture -> stopCapture -> close.

#include <cstdint>
#include <string>
#include <vector>
#include <atomic>
#include <thread>

class NativePcmBuffer;
typedef struct _snd_pcm snd_pcm_t;

struct AlsaCaptureDeviceInfo {
    std::string deviceId;   // e.g. "hw:1,0" (pass "plughw:1,0" or "default" manually if hw: refuses your format)
    std::string name;       // "<card name> — <device name>"
};

class AlsaCaptureDriver {
public:
    AlsaCaptureDriver();
    ~AlsaCaptureDriver();

    AlsaCaptureDriver(const AlsaCaptureDriver&) = delete;
    AlsaCaptureDriver& operator=(const AlsaCaptureDriver&) = delete;

    // Capture-capable PCM devices, direct hw: identifiers.
    static std::vector<AlsaCaptureDeviceInfo> enumerateCaptureDevices();

    bool open(const std::string& deviceId);

    // Negotiates rate/channels/bitDepth with the hardware. The effective
    // values (get* below) may differ from the request (set_rate_near, and
    // bit-depth falls back S32 -> S24_3LE -> S16 when the exact one is
    // refused). Interleaved integer PCM only.
    bool configureCapture(int sampleRate, int channels, int bitDepth);

    bool startCapture();

    // Non-blocking-ish drain of the internal ring (may wait up to ~100 ms
    // when empty). Returns -1 when not capturing, 0 when nothing buffered
    // yet, else bytes copied (frame-aligned).
    int readCapture(uint8_t* out, int maxBytes);

    void stopCapture();
    void close();

    int  getConfiguredCaptureRate() const     { return capRate; }
    int  getConfiguredCaptureChannels() const { return capChannels; }
    int  getConfiguredCaptureBitDepth() const { return capBitDepth; }
    // Bytes per sample on the wire (3 for S24_3LE — no padding byte).
    int  getConfiguredCaptureSubslotSize() const { return capSubslotSize; }
    bool isCapturing() const                  { return streaming.load(std::memory_order_acquire); }

private:
    void readerThreadFn();

    snd_pcm_t* pcm = nullptr;
    int capRate = 0;
    int capChannels = 0;
    int capBitDepth = 0;
    int capSubslotSize = 0;

    std::atomic<bool> streaming{false};
    std::thread readerThread;
    NativePcmBuffer* ring = nullptr;
    bool opened = false;
    bool configured = false;
};

#endif // ALSA_CAPTURE_H
