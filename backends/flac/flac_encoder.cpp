#include "flac_encoder.h"

#include <unistd.h>

#include <FLAC/stream_encoder.h>

namespace ae {

// enc_ is held as void* in the header (libFLAC's handle is an anonymous-struct
// typedef and can't be forward-declared). Cast back here.
static inline FLAC__StreamEncoder* E(void* p) {
    return static_cast<FLAC__StreamEncoder*>(p);
}

FlacEncoder::~FlacEncoder() { close(); }

bool FlacEncoder::open(int fd, const AudioFormat& fmt) {
    close();
    if (fmt.channels <= 0 || fmt.sampleRate <= 0) return false;
    channels_ = fmt.channels;
    int bits = fmt.bitDepth > 0 ? fmt.bitDepth : 16;

    int dupFd = ::dup(fd);
    if (dupFd < 0) return false;
    file_ = ::fdopen(dupFd, "wb");
    if (!file_) { ::close(dupFd); return false; }

    enc_ = FLAC__stream_encoder_new();
    if (!enc_) { std::fclose(file_); file_ = nullptr; return false; }

    FLAC__stream_encoder_set_channels(E(enc_), (unsigned)channels_);
    FLAC__stream_encoder_set_bits_per_sample(E(enc_), (unsigned)bits);
    FLAC__stream_encoder_set_sample_rate(E(enc_), (unsigned)fmt.sampleRate);
    FLAC__stream_encoder_set_compression_level(E(enc_), 5);
    FLAC__stream_encoder_set_total_samples_estimate(E(enc_), 0);   // streaming capture

    // init_FILE seeks the FILE to patch STREAMINFO/seektable on finish().
    if (FLAC__stream_encoder_init_FILE(E(enc_), file_, nullptr, nullptr)
            != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
        close();
        return false;
    }
    return true;
}

bool FlacEncoder::encode(const uint8_t* pcm, int len) {
    if (!enc_ || channels_ <= 0) return false;
    const int totalSamples = len / 2;            // interleaved int16
    const int frames = totalSamples / channels_;
    if (frames <= 0) return true;

    if ((int)scratch_.size() < totalSamples) scratch_.resize(totalSamples);
    const int16_t* src = reinterpret_cast<const int16_t*>(pcm);
    for (int i = 0; i < frames * channels_; i++) scratch_[i] = src[i];   // widen int16 -> int32

    return FLAC__stream_encoder_process_interleaved(
        E(enc_), reinterpret_cast<const FLAC__int32*>(scratch_.data()),
        (unsigned)frames) != 0;
}

void FlacEncoder::close() {
    if (enc_) {
        FLAC__stream_encoder_finish(E(enc_));     // flush + patch headers
        FLAC__stream_encoder_delete(E(enc_));
        enc_ = nullptr;
    }
    if (file_) { std::fclose(file_); file_ = nullptr; }   // also closes the dup fd
    channels_ = 0;
}

} // namespace ae
