#ifndef AE_CORE_DECODER_H
#define AE_CORE_DECODER_H

#include <cstdint>
#include "core/audio_format.h"

namespace ae {

// Decode seam: turns an encoded stream (behind a file descriptor) into
// interleaved PCM. A decoder is a decoded-PCM *source* for the engine — the
// engine's decode thread pumps read() and pushes the PCM into the ring that the
// output thread drains.
//
// Platform backends implement this: on Android, MediaCodecDecoder over
// AMediaExtractor/AMediaCodec (backends/mediacodec). core/ declares the seam and
// never includes any codec header.
//
// Lifecycle: open() -> format() -> read()* -> close().
class Decoder {
public:
    virtual ~Decoder() = default;

    // Point at an encoded stream. `offset`/`length` scope a region inside the
    // fd (length < 0 means "to end of file"). Returns false on failure.
    // The decoder does NOT take ownership of the fd; the caller closes it after
    // close(). Implementations dup() if they need it to outlive the call.
    virtual bool open(int fd, int64_t offset, int64_t length) = 0;

    // The PCM format the decoder emits (valid after a successful open()).
    virtual AudioFormat format() const = 0;

    // Pump one decode cycle: feed input, drain output. Writes up to `maxLen`
    // bytes of interleaved PCM into `out`. Returns:
    //   > 0  bytes produced,
    //     0  nothing this cycle (call again),
    //    -1  end-of-stream (no more data will ever come),
    //  < -1  fatal decode error.
    virtual int read(uint8_t* out, int maxLen) = 0;

    // Total stream duration in ms, or -1 if unknown.
    virtual int64_t durationMs() const { return -1; }

    // Seek to `positionMs`. Default: unsupported. (Phase 2c.)
    virtual bool seekMs(int64_t positionMs) { (void)positionMs; return false; }

    // Gapless trim metadata: encoder delay / padding, in frames. (Phase 2c.)
    virtual int encoderDelayFrames() const { return 0; }
    virtual int encoderPaddingFrames() const { return 0; }

    // Called by the engine once the sink has negotiated its wire format, before
    // the first read(). Lets a decoder that must match the device (e.g. the DSD
    // DoP packer, whose subslot is 3 or 4 depending on the DAC) adapt. After
    // this returns, the engine re-reads format(). Default: nothing to adapt.
    virtual void onOutputFormat(const AudioFormat& sinkFormat) { (void)sinkFormat; }

    // Release codec/extractor resources.
    virtual void close() = 0;
};

} // namespace ae

#endif // AE_CORE_DECODER_H
