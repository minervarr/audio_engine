#ifndef AE_BACKENDS_MP3_ENCODER_H
#define AE_BACKENDS_MP3_ENCODER_H

#include <cstdint>
#include <cstdio>
#include <vector>

#include "core/encoder.hpp"

namespace ae {

// Native MP3 encode via vendored LAME / libmp3lame (LGPL). Implements the
// Encoder seam: interleaved 16-bit PCM in, a VBR .mp3 stream out on a file
// descriptor. Used by the Recorder to capture mic audio to MP3. The only
// includer of <lame.h>.
//
// The encoder dup()s the fd and wraps it in a FILE* so LAME can seek back and
// patch the Xing/LAME VBR header at close().
class Mp3Encoder : public Encoder {
public:
    Mp3Encoder() = default;
    ~Mp3Encoder() override;

    bool open(int fd, const AudioFormat& fmt) override;
    bool encode(const uint8_t* pcm, int len) override;
    void close() override;

private:
    void*                gf_ = nullptr;   // lame_global_flags* (opaque)
    std::FILE*           file_ = nullptr;
    int                  channels_ = 0;
    std::vector<uint8_t> mp3buf_;   // LAME output scratch
};

} // namespace ae

#endif // AE_BACKENDS_MP3_ENCODER_H
