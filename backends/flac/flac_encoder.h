#ifndef AE_BACKENDS_FLAC_ENCODER_H
#define AE_BACKENDS_FLAC_ENCODER_H

#include <cstdint>
#include <cstdio>
#include <vector>

#include "core/encoder.hpp"

namespace ae {

// Native FLAC encode via vendored libFLAC (BSD-3-Clause). Implements the Encoder
// seam: interleaved PCM in, a .flac stream out on a file descriptor. Used by the
// Recorder to capture mic audio to FLAC. The only includer of
// <FLAC/stream_encoder.h>.
//
// Input is interleaved 16-bit PCM (the AAudio capture format). The encoder dup()s
// the fd and wraps it in a FILE* so libFLAC can seek back and patch STREAMINFO /
// the seektable at close().
class FlacEncoder : public Encoder {
public:
    FlacEncoder() = default;
    ~FlacEncoder() override;

    bool open(int fd, const AudioFormat& fmt) override;
    bool encode(const uint8_t* pcm, int len) override;
    void close() override;

private:
    void*                enc_ = nullptr;   // FLAC__StreamEncoder* (opaque)
    std::FILE*           file_ = nullptr;
    int                  channels_ = 0;
    std::vector<int32_t> scratch_;   // int16 -> int32 widening for libFLAC
};

} // namespace ae

#endif // AE_BACKENDS_FLAC_ENCODER_H
