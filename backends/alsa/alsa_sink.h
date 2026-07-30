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

    // Discard everything queued but not yet played, and re-arm the device.
    // For seeking: otherwise the hardware keeps playing the pre-seek buffer.
    void flush();

    // Frames accepted by the device but not yet heard (snd_pcm_delay). Lets the
    // caller report a true position and drain the tail at a track boundary, the
    // same role UsbAudioDriver::getPendingPlaybackMs serves.
    int  pendingFrames() const;

    // Device buffer geometry actually granted by the hardware, which can differ
    // from what was asked for.
    unsigned bufferFrames() const { return bufferFrames_; }
    unsigned periodFrames() const { return periodFrames_; }

    // Underruns recovered from. snd_pcm_recover is called in SILENT mode, so
    // without this counter a dropout leaves no trace anywhere.
    int  underrunCount() const { return underruns.load(std::memory_order_relaxed); }
    bool hasFaulted() const { return faulted.load(std::memory_order_acquire); }

private:
    void reportUnderruns();

    // The device buffer is this backend's ONLY cushion — unlike USB and JACK
    // there is no application-side ring in front of it, so the decode thread
    // writes straight into the hardware. At the previous 200 ms any producer
    // stall longer than a fifth of a second underran and produced an audible
    // stop-then-resume. Matches UsbAudioDriver::playbackRingMs (usb_audio.h:235)
    // in intent, trimmed because this is real device memory, not our own heap.
    static constexpr int kBufferMs  = 2000;
    static constexpr int kPeriodDiv = 8;      // periods per buffer -> 250 ms

    snd_pcm_t* pcm = nullptr;
    ae::AudioFormat fmt_{};
    std::atomic<bool> streaming{false};
    bool opened = false;
    bool configured = false;

    unsigned bufferFrames_ = 0;
    unsigned periodFrames_ = 0;
    std::atomic<int>  underruns{0};
    std::atomic<bool> faulted{false};
    int lastReportedUnderruns_ = 0;
    int64_t lastUnderrunReportMs_ = 0;
};

#endif // AE_BACKENDS_ALSA_SINK_H
