#ifndef AE_BACKENDS_DSD_DECODER_H
#define AE_BACKENDS_DSD_DECODER_H

#include <cstdint>
#include <vector>

#include "core/decoder.hpp"
#include "core/dsp/dsd/dsd_mode.h"
#include "dff_parser.h"
#include "dsf_parser.h"

namespace ae {

// DSD decode backend: parses a .dff/.dsf file off an fd and emits DSD-over-PCM
// (DoP 1.1) frames ready for a USB DAC. Implements the Decoder seam so the
// engine plays it like any other source — but its AudioFormat is flagged isDsd,
// so the engine bypasses EQ/gain and the USB sink raw-passthroughs the bytes.
//
// This cut ships the universal DoP path (works on any DSD-capable USB DAC). The
// DoP subslot (3 or 4 bytes) is learned from the DAC via onOutputFormat().
// fd: expects a whole-file descriptor at offset 0.
class DsdDecoder : public Decoder {
public:
    DsdDecoder() = default;
    ~DsdDecoder() override;

    bool open(int fd, int64_t offset, int64_t length) override;
    AudioFormat format() const override { return format_; }
    int read(uint8_t* out, int maxLen) override;
    int64_t durationMs() const override;
    bool seekMs(int64_t positionMs) override;
    void onOutputFormat(const AudioFormat& sinkFormat) override;
    void close() override;

private:
    int servePending(uint8_t* out, int maxLen);
    void rebuildFormat();

    int          dupFd_  = -1;
    bool         isDsf_  = false;
    bool         bitReverse_ = false;      // DSF LSB-first needs reversal
    int          dsdRate_ = 0;             // DSD bit rate (e.g. 2822400 for DSD64)
    int          channels_ = 0;
    int          blockSize_ = 0;           // per-channel block bytes
    int          dopSubslot_ = 3;          // learned from the DAC (3 or 4)
    int          dopCounter_ = 0;          // DoP marker phase, persists across reads
    AudioFormat  format_{};

    DffParser    dff_;
    DsfParser    dsf_;

    std::vector<uint8_t> left_;
    std::vector<uint8_t> right_;
    std::vector<uint8_t> packed_;          // one packed block awaiting delivery
    size_t               packedOff_ = 0;
    bool                 eos_ = false;
};

} // namespace ae

#endif // AE_BACKENDS_DSD_DECODER_H
