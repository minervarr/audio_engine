#include "flac_decoder.h"

#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cstring>

#include <FLAC/stream_decoder.h>

namespace ae {

// dec_ is held as void* in the header (libFLAC typedefs the handle from an
// anonymous struct, so it can't be forward-declared). Cast back here.
static inline FLAC__StreamDecoder* D(void* p) {
    return static_cast<FLAC__StreamDecoder*>(p);
}

// --- libFLAC C trampolines (client_data == FlacDecoder*) ---------------------

static FLAC__StreamDecoderReadStatus read_tr(
        const FLAC__StreamDecoder*, FLAC__byte buffer[], size_t* bytes, void* cd) {
    switch (static_cast<FlacDecoder*>(cd)->onRead(buffer, bytes)) {
        case 0:  return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
        case 1:  return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
        default: return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
    }
}
static FLAC__StreamDecoderSeekStatus seek_tr(
        const FLAC__StreamDecoder*, FLAC__uint64 off, void* cd) {
    switch (static_cast<FlacDecoder*>(cd)->onSeek(off)) {
        case 0:  return FLAC__STREAM_DECODER_SEEK_STATUS_OK;
        case 2:  return FLAC__STREAM_DECODER_SEEK_STATUS_UNSUPPORTED;
        default: return FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
    }
}
static FLAC__StreamDecoderTellStatus tell_tr(
        const FLAC__StreamDecoder*, FLAC__uint64* off, void* cd) {
    switch (static_cast<FlacDecoder*>(cd)->onTell(off)) {
        case 0:  return FLAC__STREAM_DECODER_TELL_STATUS_OK;
        case 2:  return FLAC__STREAM_DECODER_TELL_STATUS_UNSUPPORTED;
        default: return FLAC__STREAM_DECODER_TELL_STATUS_ERROR;
    }
}
static FLAC__StreamDecoderLengthStatus length_tr(
        const FLAC__StreamDecoder*, FLAC__uint64* len, void* cd) {
    switch (static_cast<FlacDecoder*>(cd)->onLength(len)) {
        case 0:  return FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
        case 2:  return FLAC__STREAM_DECODER_LENGTH_STATUS_UNSUPPORTED;
        default: return FLAC__STREAM_DECODER_LENGTH_STATUS_ERROR;
    }
}
static FLAC__bool eof_tr(const FLAC__StreamDecoder*, void* cd) {
    return static_cast<FlacDecoder*>(cd)->onEof() ? 1 : 0;
}
static FLAC__StreamDecoderWriteStatus write_tr(
        const FLAC__StreamDecoder*, const FLAC__Frame* frame,
        const FLAC__int32* const buffer[], void* cd) {
    static_cast<FlacDecoder*>(cd)->appendFrame(
        frame->header.channels, frame->header.bits_per_sample,
        frame->header.blocksize, buffer);
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}
static void meta_tr(const FLAC__StreamDecoder*, const FLAC__StreamMetadata* m, void* cd) {
    if (m->type == FLAC__METADATA_TYPE_STREAMINFO) {
        const auto& si = m->data.stream_info;
        static_cast<FlacDecoder*>(cd)->setStreamInfo(
            (int)si.sample_rate, (int)si.channels, (int)si.bits_per_sample,
            (int64_t)si.total_samples);
    }
}
static void error_tr(const FLAC__StreamDecoder*, FLAC__StreamDecoderErrorStatus, void*) {}

// --- FlacDecoder -------------------------------------------------------------

FlacDecoder::~FlacDecoder() { close(); }

bool FlacDecoder::open(int fd, int64_t offset, int64_t length) {
    close();
    dupFd_ = ::dup(fd);
    if (dupFd_ < 0) return false;

    regionOffset_ = offset < 0 ? 0 : offset;
    if (length < 0) {
        struct stat st;
        regionLength_ = (::fstat(dupFd_, &st) == 0) ? (st.st_size - regionOffset_) : 0;
    } else {
        regionLength_ = length;
    }
    pos_ = 0;

    dec_ = FLAC__stream_decoder_new();
    if (!dec_) { close(); return false; }
    FLAC__stream_decoder_set_md5_checking(D(dec_), false);

    if (FLAC__stream_decoder_init_stream(D(dec_), read_tr, seek_tr, tell_tr, length_tr,
            eof_tr, write_tr, meta_tr, error_tr, this)
            != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
        close();
        return false;
    }
    // Pull STREAMINFO so format() is valid before the first read().
    FLAC__stream_decoder_process_until_end_of_metadata(D(dec_));
    return format_.valid();
}

int FlacDecoder::read(uint8_t* out, int maxLen) {
    if (pendingOff_ < pending_.size()) return servePending(out, maxLen);
    if (eos_) return -1;

    pending_.clear();
    pendingOff_ = 0;
    while (pending_.empty() && !eos_) {
        if (!FLAC__stream_decoder_process_single(D(dec_))) { eos_ = true; break; }
        if (FLAC__stream_decoder_get_state(D(dec_)) == FLAC__STREAM_DECODER_END_OF_STREAM)
            eos_ = true;
    }
    if (pending_.empty()) return eos_ ? -1 : 0;
    return servePending(out, maxLen);
}

int FlacDecoder::servePending(uint8_t* out, int maxLen) {
    size_t remain = pending_.size() - pendingOff_;
    int n = (int)std::min<size_t>(remain, (size_t)maxLen);
    std::memcpy(out, pending_.data() + pendingOff_, n);
    pendingOff_ += n;
    if (pendingOff_ >= pending_.size()) { pending_.clear(); pendingOff_ = 0; }
    return n;
}

void FlacDecoder::appendFrame(int channels, int bits, int blocksize,
                              const int32_t* const buffer[]) {
    const int subslot = format_.subslotBytes > 0 ? format_.subslotBytes
                                                 : (bits <= 16 ? 2 : bits <= 24 ? 3 : 4);
    pending_.resize((size_t)blocksize * channels * subslot);
    size_t p = 0;
    for (int i = 0; i < blocksize; i++) {
        for (int ch = 0; ch < channels; ch++) {
            int32_t v = buffer[ch][i];
            for (int b = 0; b < subslot; b++) pending_[p++] = (uint8_t)((v >> (b * 8)) & 0xFF);
        }
    }
    pending_.resize(p);
    pendingOff_ = 0;
}

void FlacDecoder::setStreamInfo(int rate, int channels, int bits, int64_t totalSamples) {
    format_.sampleRate   = rate;
    format_.channels     = channels;
    format_.bitDepth     = bits;
    format_.subslotBytes = (bits <= 16) ? 2 : (bits <= 24 ? 3 : 4);
    format_.isFloat      = false;
    format_.isDsd        = false;
    totalSamples_        = totalSamples;
}

int64_t FlacDecoder::durationMs() const {
    if (totalSamples_ > 0 && format_.sampleRate > 0)
        return totalSamples_ * 1000 / format_.sampleRate;
    return -1;
}

bool FlacDecoder::seekMs(int64_t positionMs) {
    if (!dec_ || format_.sampleRate <= 0) return false;
    if (positionMs < 0) positionMs = 0;
    uint64_t sample = (uint64_t)positionMs * format_.sampleRate / 1000;
    pending_.clear();
    pendingOff_ = 0;
    eos_ = false;
    if (!FLAC__stream_decoder_seek_absolute(D(dec_), sample)) {
        FLAC__stream_decoder_flush(D(dec_));   // recover from SEEK_ERROR
        return false;
    }
    return true;
}

void FlacDecoder::close() {
    if (dec_) {
        FLAC__stream_decoder_finish(D(dec_));
        FLAC__stream_decoder_delete(D(dec_));
        dec_ = nullptr;
    }
    if (dupFd_ >= 0) { ::close(dupFd_); dupFd_ = -1; }
    pending_.clear();
    pendingOff_ = 0;
    eos_ = false;
    pos_ = 0;
}

// --- fd-region stream callbacks ----------------------------------------------

int FlacDecoder::onRead(uint8_t* buffer, size_t* bytes) {
    int64_t remaining = regionLength_ - pos_;
    if (remaining <= 0) { *bytes = 0; return 1; }
    size_t want = std::min(*bytes, (size_t)remaining);
    ssize_t got = ::pread(dupFd_, buffer, want, regionOffset_ + pos_);
    if (got < 0)  { *bytes = 0; return 2; }
    if (got == 0) { *bytes = 0; return 1; }
    pos_ += got;
    *bytes = (size_t)got;
    return 0;
}

int FlacDecoder::onSeek(uint64_t off) {
    if ((int64_t)off > regionLength_) return 1;
    pos_ = (int64_t)off;
    return 0;
}
int FlacDecoder::onTell(uint64_t* off) const   { *off = (uint64_t)pos_; return 0; }
int FlacDecoder::onLength(uint64_t* len) const { *len = (uint64_t)regionLength_; return 0; }
bool FlacDecoder::onEof() const                { return pos_ >= regionLength_; }

} // namespace ae
