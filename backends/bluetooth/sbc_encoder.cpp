#include "sbc_encoder.h"

#include <sbc/sbc.h>

#include <cstdlib>
#include <cstring>
#include <new>

namespace ae {
namespace a2dp {
namespace {

// Blocks and subbands, as counts, from the single configuration bit.
int blocksOf(uint8_t bit) {
    switch (bit) {
    case kBlocks4:  return 4;
    case kBlocks8:  return 8;
    case kBlocks12: return 12;
    case kBlocks16: return 16;
    default:        return 0;
    }
}

int subbandsOf(uint8_t bit) {
    switch (bit) {
    case kSubbands4: return 4;
    case kSubbands8: return 8;
    default:         return 0;
    }
}

}  // namespace

SbcEncoder::~SbcEncoder() { finish(); }

bool SbcEncoder::init(const SbcCaps& cfg) {
    finish();

    // A capability blob has several bits set per field and describes what a
    // device COULD do; only a configuration says what will actually be sent.
    // Handing the former to libsbc would silently encode as whatever the low
    // bits happened to mean.
    if (!cfg.isConfiguration()) return false;

    const int rate     = sbcFrequencyHz(cfg.freq);
    const int blocks   = blocksOf(cfg.blockLength);
    const int subbands = subbandsOf(cfg.subbands);
    if (rate <= 0 || blocks <= 0 || subbands <= 0) return false;

    sbc_t* s = (sbc_t*)std::calloc(1, sizeof(sbc_t));
    if (!s) return false;

    // The four A2DP bytes ARE libsbc's a2dp_sbc_t on a little-endian machine,
    // which is what sbc_init_a2dp() parses -- so the configuration BlueZ
    // accepted is handed to the encoder verbatim rather than re-derived field
    // by field, where the two could disagree.
    uint8_t wire[kSbcCapsBytes];
    writeSbcCaps(cfg, wire);
    if (sbc_init_a2dp(s, 0, wire, sizeof(wire)) < 0) {
        std::free(s);
        return false;
    }

    // Native-endian S16, because that is what the decoder hands us and what
    // AudioFormat calls 16-bit everywhere else in this engine.
    s->endian = SBC_LE;

    // The bitpool is the negotiated CEILING, and using it is the point of
    // negotiating: it is what the sink said it can decode at this rate and
    // channel mode. sbc_init_a2dp() takes min_bitpool, which would stream the
    // worst quality the device accepts.
    s->bitpool = cfg.maxBitpool;

    codeSize_    = sbc_get_codesize(s);
    frameLength_ = sbc_get_frame_length(s);
    if (codeSize_ == 0 || frameLength_ == 0) {
        sbc_finish(s);
        std::free(s);
        return false;
    }

    // blocks * subbands samples per channel, per frame. Computed rather than
    // taken from sbc_get_frame_duration(), whose millisecond resolution is
    // coarse enough to drift the pacing clock (see the header).
    frameDurationUs_ = (uint32_t)(((uint64_t)blocks * (uint64_t)subbands *
                                   1000000ull) / (uint64_t)rate);

    sbc_  = s;
    ready_ = true;
    return true;
}

void SbcEncoder::finish() {
    if (sbc_) {
        sbc_finish((sbc_t*)sbc_);
        std::free(sbc_);
        sbc_ = nullptr;
    }
    ready_ = false;
    codeSize_ = frameLength_ = 0;
    frameDurationUs_ = 0;
}

size_t SbcEncoder::encode(const uint8_t* in, uint8_t* out, size_t outCap) {
    if (!ready_ || !in || !out || outCap < frameLength_) return 0;
    ssize_t written = 0;
    const ssize_t consumed =
        sbc_encode((sbc_t*)sbc_, in, codeSize_, out, outCap, &written);
    // A short read is as wrong as an error: it means the frame boundary moved,
    // and every packet after it would carry a fraction of a frame.
    if (consumed != (ssize_t)codeSize_ || written <= 0) return 0;
    return (size_t)written;
}

}  // namespace a2dp
}  // namespace ae
