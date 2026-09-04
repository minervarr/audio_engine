#ifndef AE_BACKENDS_BLUETOOTH_BLUETOOTH_SINK_H
#define AE_BACKENDS_BLUETOOTH_BLUETOOTH_SINK_H

// A2DP playback: an ae::AudioSink whose device is a pair of headphones.
//
// The three files under it each do one thing -- a2dp_sbc.h negotiates,
// sbc_encoder.h encodes, bluez_endpoint.h owns the socket -- and this is where
// they meet: PCM in at one end, RTP/SBC packets out of an L2CAP socket at the
// other, with nothing in between that this project did not write.
//
// --- pacing, and why there is no writer thread ------------------------------
//
// An A2DP socket has no clock. Write faster than real time and the packets
// queue in the kernel until the link stalls; write slower and the headphones
// run dry. So the schedule is ours to keep, and write() keeps it by sleeping:
// it stays kBufferAheadMs ahead of where the music has got to and no further.
//
// That puts the sleeping on the DECODE thread, exactly as AlsaSink's blocking
// snd_pcm_writei does, and for the same reason -- a writer thread would need a
// ring in front of it, and a ring is a second place for the same audio to sit
// and a second thing to get wrong on flush(). The cost is the same as ALSA's:
// a decode stall longer than the cushion is audible. That is a real trade and
// it is the one this engine already makes everywhere else.
//
// --- what this backend is NOT ------------------------------------------------
//
// Not bit-perfect, and it says so: SBC is lossy, and the samples are quantised
// to 16 bits before the encoder sees them. activeFormat() reports 16, which is
// what makes the signal-chain readout name the encode instead of claiming a
// lossless chain over a radio link.
#include <cstdint>
#include <string>
#include <vector>

#include "core/audio_sink.h"

#include "a2dp_sbc.h"
#include "bluez_a2dp.h"
#include "bluez_endpoint.h"
#include "sbc_encoder.h"

namespace ae {

class BluetoothSink : public ae::AudioSink {
public:
    BluetoothSink();
    ~BluetoothSink() override;

    BluetoothSink(const BluetoothSink&) = delete;
    BluetoothSink& operator=(const BluetoothSink&) = delete;

    // Register our endpoint on the device's adapter. Does NOT take the stream:
    // that happens in configure()/start(), because until a track is chosen
    // there is no rate to negotiate for.
    bool open(const bluez::SinkDevice& device);
    void close();

    // ae::AudioSink. Interleaved S16 native-endian PCM, which is what SBC
    // takes and therefore what activeFormat() reports regardless of the source.
    bool configure(const ae::AudioFormat& fmt) override;
    bool start() override;
    int  write(const uint8_t* data, int len) override;
    void stop() override;
    void flush() override;
    int  pendingPlaybackMs() const override;
    ae::AudioFormat activeFormat() const override { return fmt_; }

    // The rates this device's SBC endpoint accepts, so the caller can pick one
    // instead of guessing -- the same role AlsaOutput::probeRates() serves.
    std::vector<int> supportedRates() const;

    // The negotiated configuration, for the signal-chain readout: which codec,
    // at what bitpool, in what channel mode. Empty before start().
    const a2dp::SbcCaps& configuration() const { return cfg_; }
    int  bitpool() const { return cfg_.maxBitpool; }
    bool streaming() const { return streaming_; }

    // Why the last open()/configure()/start() failed, in BlueZ's own words
    // where there are any. "Another application is streaming to this device"
    // is the one that matters and the one this backend exists to say out loud.
    const std::string& lastError() const { return lastError_; }

private:
    bool negotiate(int rate, int channels);
    bool sendPacket();
    void resetClock();
    int64_t nowUs() const;

    // What we tell BlueZ we can PRODUCE. Every field the format has, because
    // refusing a device's preference to save a branch here would be refusing
    // music. The bitpool ceiling is the A2DP specification's own "high
    // quality" figure for 44.1 kHz joint stereo; the sink's own maximum is
    // what actually binds, and selectSbcConfiguration() applies it.
    static a2dp::SbcCaps ourCapabilities();

    // How far ahead of real time write() runs. Below ~80 ms a scheduling hiccup
    // on the decode thread is audible; far above it every Stop and every track
    // change has that much stale audio to throw away, and Phase 1 of this work
    // was spent removing exactly that. 120 ms is the compromise, and it is what
    // pendingPlaybackMs() reports so the player drains it rather than cutting it.
    static constexpr int kBufferAheadMs = 120;

    // The RTP-SBC payload header carries the frame count in FOUR bits, so a
    // packet can never hold more than 15 frames however large the MTU is.
    static constexpr int kMaxFramesPerPacket = 15;

    bluez::SinkDevice   device_{};
    bluez::Endpoint     endpoint_;
    bluez::Transport    transport_{};
    a2dp::SbcEncoder    encoder_;
    a2dp::SbcCaps       cfg_{};
    ae::AudioFormat     fmt_{};
    std::string         lastError_;

    bool  opened_    = false;
    bool  configured_ = false;
    bool  streaming_ = false;

    // The packet under construction: an RTP header, the one-byte SBC payload
    // header, then whole frames.
    std::vector<uint8_t> packet_;
    size_t   packetFill_    = 0;
    int      packetFrames_  = 0;
    int      framesPerPacket_ = 1;

    // Partial PCM left over from a write() that did not end on a frame
    // boundary. One frame's worth at most, and it is why write() can accept any
    // length rather than demanding multiples of codeSize().
    std::vector<uint8_t> pcmCarry_;
    size_t   carryFill_ = 0;

    uint16_t seq_       = 0;
    uint32_t rtpTime_   = 0;
    uint32_t ssrc_      = 0;
    int64_t  clockStartUs_ = 0;
    int64_t  framesSent_   = 0;
};

}  // namespace ae

#endif  // AE_BACKENDS_BLUETOOTH_BLUETOOTH_SINK_H
