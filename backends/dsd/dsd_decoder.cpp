#include "dsd_decoder.h"

#include <unistd.h>
#include <algorithm>
#include <cstring>

#include "core/dsp/dsd/dsd_packager.h"

namespace ae {

namespace {
// MSB<->LSB bit-reversal table for DSF streams stored LSB-first.
const uint8_t* bitReverseTable() {
    static uint8_t table[256];
    static bool built = false;
    if (!built) {
        for (int i = 0; i < 256; i++) {
            int r = 0, v = i;
            for (int b = 0; b < 8; b++) { r = (r << 1) | (v & 1); v >>= 1; }
            table[i] = (uint8_t)r;
        }
        built = true;
    }
    return table;
}

bool magic4(int fd, const char* expect) {
    char b[4];
    if (::pread(fd, b, 4, 0) != 4) return false;
    return std::memcmp(b, expect, 4) == 0;
}
} // namespace

DsdDecoder::~DsdDecoder() { close(); }

bool DsdDecoder::open(int fd, int64_t /*offset*/, int64_t /*length*/) {
    close();
    dupFd_ = ::dup(fd);
    if (dupFd_ < 0) return false;

    if (magic4(dupFd_, "DSD ")) {           // Sony DSF
        if (!dsf_.parse(dupFd_)) { close(); return false; }
        isDsf_      = true;
        dsdRate_    = dsf_.sampleRate();
        channels_   = dsf_.channelCount();
        blockSize_  = dsf_.blockSizePerChannel();
        bitReverse_ = dsf_.bitsPerSample() == 1;
    } else if (magic4(dupFd_, "FRM8")) {    // Philips DSDIFF (.dff)
        if (!dff_.parse(dupFd_)) { close(); return false; }
        isDsf_      = false;
        dsdRate_    = dff_.sampleRate();
        channels_   = dff_.channelCount();
        blockSize_  = dff_.blockSizePerChannel();
        bitReverse_ = false;                // DFF is always MSB-first
    } else {
        close();
        return false;
    }
    if (dsdRate_ <= 0 || channels_ <= 0 || blockSize_ <= 0) { close(); return false; }

    left_.assign(blockSize_, 0);
    right_.assign(blockSize_, 0);
    // Worst case DoP at 4-byte subslot = 2 wire bytes per DSD byte per
    // channel — reserve it once here so read()'s per-block resize() below
    // never reallocates (resize() within an already-reserved capacity is
    // guaranteed non-allocating), matching the project's "never allocate on
    // the streaming loop" convention instead of relying on clear() happening
    // not to release capacity.
    packed_.reserve((size_t)blockSize_ * channels_ * 2);
    rebuildFormat();
    return format_.valid();
}

void DsdDecoder::rebuildFormat() {
    // DoP: rate = dsdRate/16, 24-bit words, subslot learned from the DAC.
    format_.sampleRate   = dsdRate_ / 16;
    format_.channels     = channels_;
    format_.bitDepth     = 24;
    format_.subslotBytes = dopSubslot_;
    format_.isFloat      = false;
    format_.isDsd        = true;
}

void DsdDecoder::onOutputFormat(const AudioFormat& sinkFormat) {
    dopSubslot_ = (sinkFormat.subslotBytes == 4) ? 4 : 3;
    rebuildFormat();
}

int DsdDecoder::servePending(uint8_t* out, int maxLen) {
    size_t remain = packed_.size() - packedOff_;
    int n = (int)std::min<size_t>(remain, (size_t)maxLen);
    std::memcpy(out, packed_.data() + packedOff_, n);
    packedOff_ += n;
    if (packedOff_ >= packed_.size()) { packed_.clear(); packedOff_ = 0; }
    return n;
}

int DsdDecoder::read(uint8_t* out, int maxLen) {
    if (packedOff_ < packed_.size()) return servePending(out, maxLen);
    if (eos_) return -1;

    bool hasData = isDsf_ ? dsf_.readBlockPair(left_.data(), right_.data())
                          : dff_.readBlockPair(left_.data(), right_.data());
    if (!hasData) { eos_ = true; return -1; }

    if (bitReverse_) {
        const uint8_t* rev = bitReverseTable();
        for (int i = 0; i < blockSize_; i++) {
            left_[i]  = rev[left_[i]];
            right_[i] = rev[right_[i]];
        }
    }

    // Capacity for this was reserved once in open() at the same worst-case
    // size, so this never reallocates.
    packed_.resize((size_t)blockSize_ * channels_ * 2);
    int packedLen = DsdPackager::packDop(left_.data(), right_.data(), blockSize_,
                                         channels_, packed_.data(), dopCounter_,
                                         dopSubslot_);
    packed_.resize(packedLen);
    packedOff_ = 0;
    if (packedLen == 0) return 0;
    return servePending(out, maxLen);
}

int64_t DsdDecoder::durationMs() const {
    int64_t samples = isDsf_ ? dsf_.totalSamples() : dff_.totalSamples();
    if (dsdRate_ <= 0) return -1;
    return samples * 1000 / dsdRate_;
}

bool DsdDecoder::seekMs(int64_t positionMs) {
    if (positionMs < 0) positionMs = 0;
    int64_t targetSample   = positionMs * dsdRate_ / 1000;
    int64_t samplesPerBlock = (int64_t)blockSize_ * 8;
    int targetBlock = (int)(targetSample / samplesPerBlock);
    if (isDsf_) dsf_.seekToBlock(targetBlock);
    else        dff_.seekToBlock(targetBlock);
    packed_.clear();
    packedOff_ = 0;
    dopCounter_ = 0;
    eos_ = false;
    return true;
}

void DsdDecoder::close() {
    if (dupFd_ >= 0) { ::close(dupFd_); dupFd_ = -1; }
    packed_.clear();
    packedOff_ = 0;
    dopCounter_ = 0;
    eos_ = false;
}

} // namespace ae
