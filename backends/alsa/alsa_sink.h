#ifndef AE_BACKENDS_ALSA_SINK_H
#define AE_BACKENDS_ALSA_SINK_H

// Direct-hardware ALSA playback backend (desktop Linux). No sound server —
// writes straight to the kernel PCM device (a raw hw: device is exclusive:
// one app at a time). Implements the shared ae::AudioSink contract:
// open -> configure -> start -> write -> stop -> close.

#include <cstdint>
#include <string>
#include <atomic>

#include "core/audio_sink.h"

typedef struct _snd_pcm snd_pcm_t;

class AlsaSink : public ae::AudioSink {
public:
    AlsaSink();
    ~AlsaSink() override;

    AlsaSink(const AlsaSink&) = delete;
    AlsaSink& operator=(const AlsaSink&) = delete;

    bool open(const std::string& deviceId);

    // ae::AudioSink. Negotiates rate/channels/bitDepth with the hardware; the
    // effective values (activeFormat()) may differ from the request (rate_near,
    // and bit-depth falls back S32 -> S24_3LE -> S16). Interleaved integer PCM.
    bool configure(const ae::AudioFormat& fmt) override;
    bool start() override;
    // Blocking write of interleaved PCM in the active format. Recovers from
    // underruns (-EPIPE) transparently. Returns bytes consumed, -1 if not
    // started.
    int  write(const uint8_t* data, int len) override;
    void stop() override;
    ae::AudioFormat activeFormat() const override { return fmt_; }

    void close();

private:
    snd_pcm_t* pcm = nullptr;
    ae::AudioFormat fmt_{};
    std::atomic<bool> streaming{false};
    bool opened = false;
    bool configured = false;
};

#endif // AE_BACKENDS_ALSA_SINK_H
