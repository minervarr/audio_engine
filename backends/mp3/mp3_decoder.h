#ifndef AE_BACKENDS_MP3_DECODER_H
#define AE_BACKENDS_MP3_DECODER_H

#include <cstddef>
#include <cstdint>

#include "core/decoder.hpp"

namespace ae {

// Native MP3 decode via vendored libmpg123 (LGPL-2.1). Implements the Decoder
// seam so the engine plays MP3 identically on every device — unlike AMediaCodec,
// whose MP3 support varies by OEM. Output is forced to interleaved signed 16-bit
// (MP3 is a lossy, ~16-bit perceptual format), so the existing subslot-2 sink
// paths carry it unchanged. The only includer of <mpg123.h>.
//
// fd is read through libmpg123's replaced reader over the region [offset, offset+
// length), so seeking works and an embedded MP3 (region inside a bigger file) is
// supported. The decoder dup()s the fd.
class Mp3Decoder : public Decoder {
public:
    Mp3Decoder() = default;
    ~Mp3Decoder() override;

    bool open(int fd, int64_t offset, int64_t length) override;
    AudioFormat format() const override { return format_; }
    int read(uint8_t* out, int maxLen) override;
    int64_t durationMs() const override;
    bool seekMs(int64_t positionMs) override;
    void close() override;

    // Handlers invoked by the libmpg123 reader trampolines (defined in the .cpp,
    // which owns all mpg123 types). Plain-typed so this header stays mpg123-free.
    //   onRead:  read up to count bytes; return bytes read, 0 at EOF, -1 on error
    //   onSeek:  whence is SEEK_SET/CUR/END; return new region-relative offset
    long onRead(void* buffer, unsigned long count);
    long onSeek(long offset, int whence);

private:
    void    refreshFormat();   // pull rate/channels after open or a format change

    void*   mh_ = nullptr;     // mpg123_handle* (opaque)
    int     dupFd_ = -1;
    int64_t regionOffset_ = 0;
    int64_t regionLength_ = 0;
    int64_t pos_ = 0;          // byte offset within the region

    AudioFormat format_{};
    bool        eos_ = false;
};

} // namespace ae

#endif // AE_BACKENDS_MP3_DECODER_H
