#ifndef AE_BACKENDS_AAUDIO_SINK_H
#define AE_BACKENDS_AAUDIO_SINK_H

#include <atomic>
#include <cstdint>
#include <vector>

#include "core/audio_sink.h"
#include "core/dsp/audio_convert.h"   // DitherLCG + floatToInt16Dither

// Matches the NDK's own declaration (typedef, not a plain struct tag) so this
// header and <aaudio/AAudio.h> agree.
typedef struct AAudioStreamStruct AAudioStream;

namespace ae {

// Android on-device output backend over AAudio (API 26+). Replaces the Java
// AudioTrackOutput. The only includer of <aaudio/AAudio.h>.
//
// Phase 2a: 16-bit PCM, MODE_STREAM blocking writes, shared/none performance.
class AAudioSink : public AudioSink {
public:
    AAudioSink() = default;
    ~AAudioSink() override;

    bool configure(const AudioFormat& fmt) override;
    bool start() override;
    int  write(const uint8_t* data, int len) override;
    void stop() override;
    AudioFormat activeFormat() const override { return format_; }

    void pause() override;
    void resume() override;
    void flush() override;
    int  pendingPlaybackMs() const override;

    // Frames the device has actually PLAYED since the stream started.
    //
    // The honest basis for an A/V clock, and not the same question
    // pendingPlaybackMs() answers. A consumer can subtract that from its own
    // written count, but only if both were sampled at the same instant — and
    // on a player they are not: the audio thread writes while the render
    // thread reads. A video player built that way saw its master clock run
    // BACKWARDS by tens of milliseconds, which stalls the frame scheduler and
    // reads on screen as a freeze every second or so.
    //
    // This counter only ever increases (until flush), so a clock derived from
    // it cannot go backwards no matter when it is read.
    int64_t framesPlayed() const;

    // Total frames handed to the stream so far.
    //
    // The anchor a video clock needs. framesPlayed() and presentedFrames()
    // below count in the same space — frames since the stream started — but
    // neither says which of them corresponds to a given piece of AUDIO. Read
    // this immediately BEFORE writing the first buffer of a segment and it
    // does: that buffer's first sample is frame `framesWritten()`.
    int64_t framesWritten() const;

    // What the DAC has actually PRESENTED, and the CLOCK_MONOTONIC instant at
    // which that was true. Returns false when the device cannot say yet, which
    // it cannot for the first few hundred milliseconds of a stream.
    //
    // Different from framesPlayed(), and the difference is the point.
    // getFramesRead() counts frames the STREAM has consumed from its buffer;
    // those frames have not reached the speaker yet, and the gap between the
    // two is the device's output latency — tens of milliseconds on a phone. A
    // video clock built on framesPlayed() therefore runs that far AHEAD of the
    // sound, which nobody can see and everybody can feel.
    //
    // `atNanos` is not decoration: the reading describes a moment already in
    // the past, so a consumer that treats `frames` as "now" trades one
    // constant error for a larger varying one.
    bool presentedFrames(int64_t& frames, int64_t& atNanos) const;

    // The stream is gone: the route changed under it.
    //
    // Plugging in headphones, connecting Bluetooth, or docking does not
    // reconfigure an AAudio stream — it DISCONNECTS it. The stream stays open
    // and useless: writes fail, and the position counters stop advancing while
    // getTimestamp keeps cheerfully answering with the last reading it had.
    //
    // For a music player that is a dropout. For a video player it is worse
    // than that, because the audio position IS the master clock: it stops
    // answering, the timeline freezes, and the picture stops with it. Nothing
    // reports an error anywhere; playback simply halts.
    //
    // Set from AAudio's own error callback, which arrives on a thread of its
    // own, and also from a failed write — the callback is the reliable signal
    // but the write result is the immediate one. Cleared by configure().
    //
    // Recovering is the CONSUMER's job, not this class's: a new stream has to
    // be opened and the consumer's own timeline re-anchored against it, and
    // only the consumer knows what it was playing. Do not stop or close the
    // stream from inside the callback — AAudio forbids it.
    bool disconnected() const { return disconnected_.load(); }

private:
    void closeStream();
    // Diagnostics for a stream that reports success and consumes nothing.
    void reportStall(int framesAccepted);
    // Rebuild the stream after the OS moved the route out from under it.
    bool reopenAfterDisconnect();
    // What configure() was ASKED for, which is what a rebuilt stream must be
    // given again -- not what the previous stream happened to be granted.
    ae::AudioFormat requested_{};
    int  starvedWrites_ = 0;
    // pendingPlaybackMs() is const and this only counts log lines.
    mutable int overPending_ = 0;

    // Pause the stream and discard everything the device has not rendered yet,
    // waiting out both asynchronous transitions. Returns false (and logs) if
    // AAudio refused either step — a silent failure here is what made flush()
    // a no-op for as long as it existed. Leaves the stream in FLUSHED; the
    // caller decides whether to re-arm it (flush) or stop it (stop).
    bool pauseAndDiscard();

    std::atomic<bool> disconnected_{false};

    AAudioStream* stream_ = nullptr;
    AudioFormat   format_{};

    // The speaker stream is always 16-bit. When the engine feeds a higher-depth
    // source (e.g. 24-bit FLAC), write() down-converts to 16-bit with persistent
    // TPDF dither. srcSubslot_ is the source's bytes/sample (2 or 3).
    int                  srcSubslot_ = 2;
    DitherLCG            dither_;
    std::vector<float>   f32_;   // 24-bit -> float scratch
    std::vector<int16_t> i16_;   // dithered 16-bit scratch
};

} // namespace ae

#endif // AE_BACKENDS_AAUDIO_SINK_H
