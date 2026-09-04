#include "a2dp_sbc.h"

namespace ae {
namespace a2dp {
namespace {

// Exactly one bit set: x != 0 and x has no lower bit than its highest.
bool oneBit(uint8_t x) { return x != 0 && (x & (uint8_t)(x - 1)) == 0; }

// Pick the single best bit out of `available`, trying `order` in turn.
// Returns 0 when none of them is offered.
uint8_t preferOneOf(uint8_t available, const uint8_t* order, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (available & order[i]) return order[i];
    return 0;
}

}  // namespace

bool SbcCaps::isConfiguration() const {
    return oneBit(freq) && oneBit(channelMode) && oneBit(blockLength) &&
           oneBit(subbands) && oneBit(allocation);
}

// The A2DP byte layout, written out as shifts rather than a packed struct: a
// C bitfield's ordering within a byte is implementation-defined, and this is a
// wire format that has to match a device across a radio link.
SbcCaps parseSbcCaps(const uint8_t* bytes, size_t len) {
    SbcCaps c;
    if (!bytes || len < kSbcCapsBytes) return c;
    c.freq        = (uint8_t)((bytes[0] >> 4) & 0x0F);
    c.channelMode = (uint8_t)(bytes[0] & 0x0F);
    c.blockLength = (uint8_t)((bytes[1] >> 4) & 0x0F);
    c.subbands    = (uint8_t)((bytes[1] >> 2) & 0x03);
    c.allocation  = (uint8_t)(bytes[1] & 0x03);
    c.minBitpool  = bytes[2];
    c.maxBitpool  = bytes[3];
    return c;
}

void writeSbcCaps(const SbcCaps& c, uint8_t out[4]) {
    out[0] = (uint8_t)(((c.freq & 0x0F) << 4) | (c.channelMode & 0x0F));
    out[1] = (uint8_t)(((c.blockLength & 0x0F) << 4) |
                       ((c.subbands & 0x03) << 2) |
                       (c.allocation & 0x03));
    out[2] = c.minBitpool;
    out[3] = c.maxBitpool;
}

int sbcFrequencyHz(uint8_t freqBit) {
    switch (freqBit) {
    case kFreq16000: return 16000;
    case kFreq32000: return 32000;
    case kFreq44100: return 44100;
    case kFreq48000: return 48000;
    default:         return 0;
    }
}

int sbcChannels(uint8_t channelModeBit) {
    switch (channelModeBit) {
    case kChanMono:         return 1;
    case kChanDual:
    case kChanStereo:
    case kChanJointStereo:  return 2;
    default:                return 0;
    }
}

bool selectSbcConfiguration(const SbcCaps& sinkCaps, int wantedRate,
                            int wantedChannels, SbcCaps& out) {
    out = SbcCaps{};

    // --- frequency ---------------------------------------------------------
    // The source's own rate first, and only then the highest the sink offers.
    // Resampling to reach a rate the sink already accepts would alter every
    // sample to no purpose, which this project refuses everywhere else.
    uint8_t wantedBit = 0;
    switch (wantedRate) {
    case 16000: wantedBit = kFreq16000; break;
    case 32000: wantedBit = kFreq32000; break;
    case 44100: wantedBit = kFreq44100; break;
    case 48000: wantedBit = kFreq48000; break;
    default:    wantedBit = 0;          break;
    }
    if (wantedBit && (sinkCaps.freq & wantedBit)) {
        out.freq = wantedBit;
    } else {
        static const uint8_t order[] = { kFreq48000, kFreq44100, kFreq32000, kFreq16000 };
        out.freq = preferOneOf(sinkCaps.freq, order, sizeof(order));
    }
    if (!out.freq) return false;

    // --- channel mode ------------------------------------------------------
    // JOINT stereo before plain stereo: at the same bitpool it spends fewer
    // bits on what the two channels have in common, which is most of the
    // signal on most music. Dual channel is two independent mono streams and
    // is worse for anything correlated; it is here only as a fallback.
    if (wantedChannels <= 1) {
        static const uint8_t order[] = { kChanMono, kChanJointStereo, kChanStereo, kChanDual };
        out.channelMode = preferOneOf(sinkCaps.channelMode, order, sizeof(order));
    } else {
        static const uint8_t order[] = { kChanJointStereo, kChanStereo, kChanDual, kChanMono };
        out.channelMode = preferOneOf(sinkCaps.channelMode, order, sizeof(order));
    }
    if (!out.channelMode) return false;

    // --- block length, subbands, allocation --------------------------------
    // The largest analysis window the format has (16 blocks, 8 subbands): more
    // of the bitpool goes to audio and less to per-frame header, and the
    // frequency resolution is finer. LOUDNESS over SNR is the A2DP
    // specification's own recommendation.
    {
        static const uint8_t order[] = { kBlocks16, kBlocks12, kBlocks8, kBlocks4 };
        out.blockLength = preferOneOf(sinkCaps.blockLength, order, sizeof(order));
    }
    {
        static const uint8_t order[] = { kSubbands8, kSubbands4 };
        out.subbands = preferOneOf(sinkCaps.subbands, order, sizeof(order));
    }
    {
        static const uint8_t order[] = { kAllocLoudness, kAllocSnr };
        out.allocation = preferOneOf(sinkCaps.allocation, order, sizeof(order));
    }
    if (!out.blockLength || !out.subbands || !out.allocation) return false;

    // --- bitpool -----------------------------------------------------------
    // The sink's own maximum, clamped to what SBC actually allows. Bitpool IS
    // the bitrate knob: taking the sink's maximum is taking the best it said it
    // could do, and lowering it is a quality decision nobody asked for.
    //
    // 250 is the format's ceiling. The clamp matters because a device is free
    // to advertise nonsense and a bitpool past the limit produces frames the
    // sink will drop.
    out.minBitpool = sinkCaps.minBitpool;
    out.maxBitpool = sinkCaps.maxBitpool > 250 ? 250 : sinkCaps.maxBitpool;
    if (out.maxBitpool < out.minBitpool) return false;
    return true;
}

// RFC 3550, and A2DP uses it unchanged. Version 2, no padding, no extension,
// no CSRCs, no marker -- so the first two bytes are constant and only the
// sequence, the timestamp and the SSRC move.
void writeRtpHeader(uint8_t* out, uint16_t seq, uint32_t timestamp, uint32_t ssrc) {
    if (!out) return;
    out[0] = 0x80;                                   // V=2, P=0, X=0, CC=0
    out[1] = kRtpPayloadTypeSbc;                     // M=0, PT=96
    out[2] = (uint8_t)((seq >> 8) & 0xFF);           // network order throughout
    out[3] = (uint8_t)(seq & 0xFF);
    out[4] = (uint8_t)((timestamp >> 24) & 0xFF);
    out[5] = (uint8_t)((timestamp >> 16) & 0xFF);
    out[6] = (uint8_t)((timestamp >> 8) & 0xFF);
    out[7] = (uint8_t)(timestamp & 0xFF);
    out[8]  = (uint8_t)((ssrc >> 24) & 0xFF);
    out[9]  = (uint8_t)((ssrc >> 16) & 0xFF);
    out[10] = (uint8_t)((ssrc >> 8) & 0xFF);
    out[11] = (uint8_t)(ssrc & 0xFF);
}

}  // namespace a2dp
}  // namespace ae
