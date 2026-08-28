#ifndef AE_BACKENDS_AAUDIO_SINK_H
#define AE_BACKENDS_AAUDIO_SINK_H

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

private:
    void closeStream();

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
