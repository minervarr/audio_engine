#ifndef AE_BACKENDS_ALSA_SOURCE_H
#define AE_BACKENDS_ALSA_SOURCE_H

// Direct-hardware ALSA capture backend (desktop Linux). No sound server
// involved — talks straight to the kernel driver, Audacity-style. Implements
// the shared ae::AudioSource contract: open -> configure -> start -> poll read
// -> stop -> close.

#include <cstdint>
#include <string>
#include <vector>
#include <atomic>
#include <thread>

#include "core/audio_source.h"

class NativePcmBuffer;
typedef struct _snd_pcm snd_pcm_t;

struct AlsaCaptureDeviceInfo {
    std::string deviceId;   // e.g. "hw:1,0" (pass "plughw:1,0" or "default" manually if hw: refuses your format)
    std::string name;       // "<card name> — <device name>"
};

class AlsaSource : public ae::AudioSource {
public:
    AlsaSource();
    ~AlsaSource() override;

    AlsaSource(const AlsaSource&) = delete;
    AlsaSource& operator=(const AlsaSource&) = delete;

    // Capture-capable PCM devices, direct hw: identifiers.
    static std::vector<AlsaCaptureDeviceInfo> enumerateCaptureDevices();

    bool open(const std::string& deviceId);

    // ae::AudioSource. Negotiates rate/channels/bitDepth with the hardware; the
    // effective values (activeFormat()) may differ from the request
    // (set_rate_near, and bit-depth falls back S32 -> S24_3LE -> S16 when the
    // exact one is refused). Interleaved integer PCM only.
    bool configure(const ae::AudioFormat& fmt) override;
    bool start() override;
    // Non-blocking-ish drain of the internal ring (may wait up to ~100 ms when
    // empty). Returns -1 when not capturing, 0 when nothing buffered yet, else
    // bytes copied (frame-aligned).
    int  read(uint8_t* out, int maxBytes) override;
    void stop() override;
    ae::AudioFormat activeFormat() const override {
        return { capRate, capChannels, capBitDepth, capSubslotSize, false };
    }

    void close();

    bool isCapturing() const { return streaming.load(std::memory_order_acquire); }

private:
    void readerThreadFn();

    snd_pcm_t* pcm = nullptr;
    int capRate = 0;
    int capChannels = 0;
    int capBitDepth = 0;
    int capSubslotSize = 0;   // bytes/sample on the wire (3 for S24_3LE — no pad byte)

    std::atomic<bool> streaming{false};
    std::thread readerThread;
    NativePcmBuffer* ring = nullptr;
    bool opened = false;
    bool configured = false;
};

#endif // AE_BACKENDS_ALSA_SOURCE_H
