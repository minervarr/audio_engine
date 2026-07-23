#ifndef AE_BACKENDS_AAUDIO_SINK_H
#define AE_BACKENDS_AAUDIO_SINK_H

#include "core/audio_sink.h"

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

private:
    void closeStream();

    AAudioStream* stream_ = nullptr;
    AudioFormat   format_{};
};

} // namespace ae

#endif // AE_BACKENDS_AAUDIO_SINK_H
