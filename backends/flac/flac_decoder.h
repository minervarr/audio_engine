#ifndef AE_BACKENDS_FLAC_DECODER_H
#define AE_BACKENDS_FLAC_DECODER_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/decoder.hpp"

namespace ae {

// Native FLAC decode via vendored libFLAC (BSD-3-Clause). Implements the Decoder
// seam so the engine plays FLAC identically on every device — unlike AMediaCodec,
// whose FLAC support and bit-depth handling vary by OEM. Emits the file's native
// bit depth (16-bit -> int16 LE, 24-bit -> 3-byte packed LE) so the USB DAC path
// stays bit-perfect. The only includer of <FLAC/stream_decoder.h>.
//
// fd is read through libFLAC's stream callbacks over the region [offset, offset+
// length), so seeking works and an embedded FLAC (region inside a bigger file) is
// supported. The decoder dup()s the fd.
class FlacDecoder : public Decoder {
public:
    FlacDecoder() = default;
    ~FlacDecoder() override;

    bool open(int fd, int64_t offset, int64_t length) override;
    AudioFormat format() const override { return format_; }
    int read(uint8_t* out, int maxLen) override;
    int64_t durationMs() const override;
    bool seekMs(int64_t positionMs) override;
    void close() override;

    // Handlers invoked by the libFLAC C trampolines (defined in the .cpp, which
    // owns all FLAC types). Plain-typed so this header stays FLAC-free.
    //   onRead:   fill up to *bytes; set *bytes to actual; 0=continue 1=eof 2=abort
    //   onSeek/onTell/onLength: 0=ok 1=error 2=unsupported
    int  onRead(uint8_t* buffer, size_t* bytes);
    int  onSeek(uint64_t off);
    int  onTell(uint64_t* off) const;
    int  onLength(uint64_t* len) const;
    bool onEof() const;
    void appendFrame(int channels, int bits, int blocksize, const int32_t* const buffer[]);
    void setStreamInfo(int rate, int channels, int bits, int64_t totalSamples);

private:
    int servePending(uint8_t* out, int maxLen);

    void*    dec_ = nullptr;   // FLAC__StreamDecoder* (opaque; anonymous-struct typedef)
    int      dupFd_ = -1;
    int64_t  regionOffset_ = 0;
    int64_t  regionLength_ = 0;
    int64_t  pos_ = 0;                 // byte offset within the region

    AudioFormat format_{};
    int64_t     totalSamples_ = 0;

    std::vector<uint8_t> pending_;
    size_t               pendingOff_ = 0;
    bool                 eos_ = false;
};

} // namespace ae

#endif // AE_BACKENDS_FLAC_DECODER_H
