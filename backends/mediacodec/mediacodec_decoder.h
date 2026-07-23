#ifndef AE_BACKENDS_MEDIACODEC_DECODER_H
#define AE_BACKENDS_MEDIACODEC_DECODER_H

#include <cstdint>
#include <vector>

#include "core/decoder.hpp"

struct AMediaExtractor;
struct AMediaCodec;

namespace ae {

// Android decode backend: AMediaExtractor pulls encoded samples off a file
// descriptor, AMediaCodec decodes them to interleaved 16-bit PCM. The only
// includer of <media/NdkMediaCodec.h> / <media/NdkMediaExtractor.h>.
//
// Phase 2a: emits ENCODING_PCM_16BIT. (Float-precision path is a later step.)
class MediaCodecDecoder : public Decoder {
public:
    MediaCodecDecoder() = default;
    ~MediaCodecDecoder() override;

    bool open(int fd, int64_t offset, int64_t length) override;
    AudioFormat format() const override { return format_; }
    int read(uint8_t* out, int maxLen) override;
    int64_t durationMs() const override { return durationMs_; }
    bool seekMs(int64_t positionMs) override;
    int encoderDelayFrames() const override { return delayFrames_; }
    int encoderPaddingFrames() const override { return paddingFrames_; }
    void close() override;

private:
    int servePending(uint8_t* out, int maxLen);

    AMediaExtractor* extractor_ = nullptr;
    AMediaCodec*     codec_     = nullptr;
    int              dupFd_     = -1;      // our own fd copy; closed in close()
    AudioFormat      format_{};
    int64_t          durationMs_ = -1;
    int              delayFrames_   = 0;   // gapless: encoder delay at file start
    int              paddingFrames_ = 0;   // gapless: encoder padding at file end
    bool             inputDone_  = false;
    bool             outputDone_ = false;

    // Leftover PCM from an output buffer larger than the caller's read chunk.
    std::vector<uint8_t> pending_;
    size_t               pendingOff_ = 0;
};

} // namespace ae

#endif // AE_BACKENDS_MEDIACODEC_DECODER_H
