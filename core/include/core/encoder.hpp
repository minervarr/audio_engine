#ifndef AE_CORE_ENCODER_H
#define AE_CORE_ENCODER_H

#include <cstdint>
#include "core/audio_format.h"

namespace ae {

// Encode seam: the mirror of Decoder. Takes interleaved PCM (from a capture
// AudioSource) and writes an encoded stream to a file descriptor. Platform
// backends implement it (Android: FlacEncoder over libFLAC); core/ declares the
// seam and includes no codec header.
//
// Lifecycle: open() -> encode()* -> close().
class Encoder {
public:
    virtual ~Encoder() = default;

    // Begin an encoded stream on `fd` in the given input PCM format. The encoder
    // dup()s the fd if it needs it to outlive the call. Returns false on failure.
    virtual bool open(int fd, const AudioFormat& fmt) = 0;

    // Encode `len` bytes of interleaved PCM in the format passed to open().
    // Returns false on a hard encode error.
    virtual bool encode(const uint8_t* pcm, int len) = 0;

    // Flush, finalize headers, and release resources.
    virtual void close() = 0;
};

} // namespace ae

#endif // AE_CORE_ENCODER_H
