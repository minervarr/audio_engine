#ifndef AE_BACKENDS_BLUETOOTH_SBC_ENCODER_H
#define AE_BACKENDS_BLUETOOTH_SBC_ENCODER_H

// libsbc, wrapped so nothing above it has to include <sbc/sbc.h>.
//
// Kept out of a2dp_sbc.h on purpose: that header is PURE and a2dp_sbc_test
// links it alone, with no library behind it. The negotiation can therefore be
// asserted against capability bytes read off real hardware on any machine,
// while THIS file is the only thing in the backend that needs libsbc present.
//
// One SBC frame is a fixed trade: codeSize() bytes of S16 PCM in, exactly
// frameLength() bytes out, worth frameDurationUs() of music. Those three
// numbers come from the negotiated configuration and are what the packetiser
// and the pacing clock are both built on -- neither guesses.
#include <cstddef>
#include <cstdint>

#include "a2dp_sbc.h"

namespace ae {
namespace a2dp {

class SbcEncoder {
public:
    SbcEncoder() = default;
    ~SbcEncoder();

    SbcEncoder(const SbcEncoder&) = delete;
    SbcEncoder& operator=(const SbcEncoder&) = delete;

    // `cfg` must be a CONFIGURATION (one bit per field) -- what
    // selectSbcConfiguration() produced and BlueZ accepted. Its bitpool is
    // taken from maxBitpool, which is the negotiated ceiling and the whole
    // point of having negotiated one.
    bool init(const SbcCaps& cfg);
    void finish();
    bool ready() const { return ready_; }

    // Bytes of interleaved S16 native-endian PCM one encode() call consumes.
    size_t codeSize() const { return codeSize_; }
    // Bytes one encode() call produces. Fixed for a given configuration, which
    // is what lets a packet be filled to the MTU without a trial encode.
    size_t frameLength() const { return frameLength_; }
    // How long one frame plays, in microseconds. libsbc reports milliseconds;
    // at 44.1 kHz a 16-block/8-subband frame is 2.9 ms, so milliseconds alone
    // would drift the pacing clock by ~3% -- an audible slide over a track.
    // Derived from the block/subband counts instead, exactly.
    uint32_t frameDurationUs() const { return frameDurationUs_; }

    // Encode exactly one frame. `in` must hold codeSize() bytes and `out` at
    // least frameLength(). Returns bytes written, or 0 on failure.
    size_t encode(const uint8_t* in, uint8_t* out, size_t outCap);

private:
    void*    sbc_ = nullptr;      // sbc_t*, hidden so the header stays clean
    bool     ready_ = false;
    size_t   codeSize_ = 0;
    size_t   frameLength_ = 0;
    uint32_t frameDurationUs_ = 0;
};

}  // namespace a2dp
}  // namespace ae

#endif  // AE_BACKENDS_BLUETOOTH_SBC_ENCODER_H
