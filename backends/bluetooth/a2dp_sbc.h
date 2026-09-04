#ifndef AE_BACKENDS_BLUETOOTH_A2DP_SBC_H
#define AE_BACKENDS_BLUETOOTH_A2DP_SBC_H

// SBC over A2DP: the capability negotiation and the wire framing.
//
// Everything in this header is PURE except encoderInit/encode, which call
// libsbc. The negotiation is what BlueZ asks us for in
// MediaEndpoint1.SelectConfiguration(): the headphones publish what they can
// do, and we answer with one configuration chosen out of it. Getting that
// wrong is not a crash, it is a device that refuses to stream or streams
// something quieter than it could have, so it is separated out here and
// tested against capability bytes read off real hardware.
//
// SBC is mandatory in A2DP -- every sink must implement it -- which is why it
// is the codec this backend ships first. LDAC and aptX are vendor codecs and
// slot in beside this file behind the same two functions.
//
// Byte layout is A2DP's own (a2dp_sbc_t in BlueZ's a2dp-codecs.h), and the
// bitfields are stated as masks rather than C bitfields on purpose: a packed
// bitfield's order is implementation-defined, and this is a wire format.
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ae {
namespace a2dp {

// --- the four capability bytes ---------------------------------------------

// Byte 0, high nibble.
enum SbcFreq : uint8_t {
    kFreq48000 = 1 << 0,
    kFreq44100 = 1 << 1,
    kFreq32000 = 1 << 2,
    kFreq16000 = 1 << 3,
};
// Byte 0, low nibble.
enum SbcChannelMode : uint8_t {
    kChanJointStereo = 1 << 0,
    kChanStereo      = 1 << 1,
    kChanDual        = 1 << 2,
    kChanMono        = 1 << 3,
};
// Byte 1, high nibble.
enum SbcBlockLength : uint8_t {
    kBlocks16 = 1 << 0,
    kBlocks12 = 1 << 1,
    kBlocks8  = 1 << 2,
    kBlocks4  = 1 << 3,
};
// Byte 1, bits 3..2.
enum SbcSubbands : uint8_t {
    kSubbands8 = 1 << 0,
    kSubbands4 = 1 << 1,
};
// Byte 1, bits 1..0.
enum SbcAllocation : uint8_t {
    kAllocLoudness = 1 << 0,
    kAllocSnr      = 1 << 1,
};

// One SBC capability or configuration blob. In a CAPABILITY every field is a
// set of bits ("all of these are supported"); in a CONFIGURATION exactly one
// bit is set in each ("this is what we will use"). Same four bytes either way,
// which is A2DP's design and not a shortcut here.
struct SbcCaps {
    uint8_t freq        = 0;
    uint8_t channelMode = 0;
    uint8_t blockLength = 0;
    uint8_t subbands    = 0;
    uint8_t allocation  = 0;
    uint8_t minBitpool  = 0;
    uint8_t maxBitpool  = 0;

    // Exactly one bit set in each of the five fields.
    bool isConfiguration() const;
};

// The four bytes as they travel over D-Bus, both ways.
SbcCaps parseSbcCaps(const uint8_t* bytes, size_t len);
void    writeSbcCaps(const SbcCaps& c, uint8_t out[4]);
constexpr size_t kSbcCapsBytes = 4;

// Choose what to stream, out of what the sink says it can take.
//
// Returns false when there is no overlap at all -- a sink that offers no
// frequency we can produce, say -- which is the honest answer rather than a
// configuration nobody can play.
//
// The preferences are the ones every A2DP source converges on, and each is a
// real trade rather than a default: the HIGHEST shared sample rate (never
// resample if the sink can take the source rate); JOINT stereo, which spends
// the same bitpool better on correlated channels than plain stereo; 16 blocks
// and 8 subbands, the largest analysis window the format has, so the bitpool
// buys resolution instead of header; and LOUDNESS allocation, which is what
// the A2DP specification itself recommends over SNR.
bool selectSbcConfiguration(const SbcCaps& sinkCaps, int wantedRate,
                            int wantedChannels, SbcCaps& out);

// Hz for the single frequency bit set in a configuration, or 0.
int sbcFrequencyHz(uint8_t freqBit);
// 1 or 2 for the channel-mode bit set in a configuration, or 0.
int sbcChannels(uint8_t channelModeBit);

// --- the wire ---------------------------------------------------------------

// An A2DP media packet is an RTP header followed by a codec payload. For SBC
// the payload opens with one more byte saying how many SBC frames follow.
constexpr size_t kRtpHeaderBytes    = 12;
constexpr size_t kSbcPayloadHeaderBytes = 1;
constexpr uint8_t kRtpPayloadTypeSbc = 96;   // the dynamic type A2DP uses

// Write the 12-byte RTP header for one media packet. `timestamp` counts in
// FRAMES (samples per channel), not milliseconds, and `seq` wraps naturally.
void writeRtpHeader(uint8_t* out, uint16_t seq, uint32_t timestamp, uint32_t ssrc);

}  // namespace a2dp
}  // namespace ae

#endif  // AE_BACKENDS_BLUETOOTH_A2DP_SBC_H
