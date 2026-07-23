#ifndef AE_BACKENDS_AAUDIO_SOURCE_H
#define AE_BACKENDS_AAUDIO_SOURCE_H

#include "core/audio_source.h"

// Matches the NDK's own declaration (typedef, not a plain struct tag) so this
// header and <aaudio/AAudio.h> agree.
typedef struct AAudioStreamStruct AAudioStream;

namespace ae {

// Android on-device capture backend over AAudio (API 26+). Replaces the Java
// AudioRecordInput. The engine pulls PCM out via read(). The only includer of
// <aaudio/AAudio.h> on the input side.
//
// 16-bit PCM, MODE_STREAM blocking reads. The caller (host app) is responsible
// for the RECORD_AUDIO runtime permission.
class AAudioSource : public AudioSource {
public:
    AAudioSource() = default;
    ~AAudioSource() override;

    bool configure(const AudioFormat& fmt) override;
    bool start() override;
    int  read(uint8_t* out, int maxLen) override;
    void stop() override;
    AudioFormat activeFormat() const override { return format_; }

private:
    void closeStream();

    AAudioStream* stream_ = nullptr;
    AudioFormat   format_{};
};

} // namespace ae

#endif // AE_BACKENDS_AAUDIO_SOURCE_H
