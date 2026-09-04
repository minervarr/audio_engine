#include "bluetooth_sink.h"

#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <thread>

namespace ae {
namespace {

// The device's transport goes through this many states before it carries
// audio, and BlueZ drives them asynchronously. Long enough for a headset to
// wake its radio, short enough that a device that is not coming back does not
// hold the UI.
constexpr int kNegotiateTimeoutMs = 6000;

void btLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void btLog(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::printf("[Bluetooth] ");
    std::vprintf(fmt, ap);
    std::printf("\n");
    va_end(ap);
    std::fflush(stdout);
}

}  // namespace

BluetoothSink::BluetoothSink() {
    // A random-enough SSRC. RTP wants it unique per source; nothing on an A2DP
    // link multiplexes sources, so any stable value does -- this one just
    // avoids two players on one machine picking the same number.
    ssrc_ = (uint32_t)(uintptr_t)this ^ (uint32_t)::getpid();
}

BluetoothSink::~BluetoothSink() { close(); }

a2dp::SbcCaps BluetoothSink::ourCapabilities() {
    a2dp::SbcCaps c;
    c.freq        = a2dp::kFreq16000 | a2dp::kFreq32000 |
                    a2dp::kFreq44100 | a2dp::kFreq48000;
    c.channelMode = a2dp::kChanMono | a2dp::kChanDual |
                    a2dp::kChanStereo | a2dp::kChanJointStereo;
    c.blockLength = a2dp::kBlocks4 | a2dp::kBlocks8 |
                    a2dp::kBlocks12 | a2dp::kBlocks16;
    c.subbands    = a2dp::kSubbands4 | a2dp::kSubbands8;
    c.allocation  = a2dp::kAllocSnr | a2dp::kAllocLoudness;
    c.minBitpool  = 2;
    c.maxBitpool  = 53;
    return c;
}

int64_t BluetoothSink::nowUs() const {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000ll + ts.tv_nsec / 1000;
}

bool BluetoothSink::open(const bluez::SinkDevice& device) {
    close();
    lastError_.clear();

    if (device.path.empty() || device.adapter.empty()) {
        lastError_ = "no such Bluetooth device";
        return false;
    }
    if (!device.hasSbc) {
        // Every A2DP sink must implement SBC, so this is BlueZ still
        // discovering the endpoints rather than a device that lacks it.
        lastError_ = device.name + " has published no SBC endpoint yet";
        return false;
    }

    device_ = device;
    if (!endpoint_.start(device.adapter, ourCapabilities())) {
        lastError_ = endpoint_.lastError();
        if (lastError_.empty()) lastError_ = "could not register an A2DP endpoint";
        return false;
    }
    opened_ = true;
    btLog("endpoint registered on %s for %s [%s]", device.adapter.c_str(),
         device.name.c_str(), device.address.c_str());
    return true;
}

void BluetoothSink::close() {
    stop();
    endpoint_.stop();
    opened_ = false;
    configured_ = false;
    cfg_ = a2dp::SbcCaps{};
    fmt_ = ae::AudioFormat{};
}

std::vector<int> BluetoothSink::supportedRates() const {
    std::vector<int> out;
    const uint8_t f = device_.sbcCaps.freq;
    if (f & a2dp::kFreq16000) out.push_back(16000);
    if (f & a2dp::kFreq32000) out.push_back(32000);
    if (f & a2dp::kFreq44100) out.push_back(44100);
    if (f & a2dp::kFreq48000) out.push_back(48000);
    return out;
}

// Make BlueZ configure US, at this rate.
//
// The profile is taken DOWN and brought back up, because SelectConfiguration is
// only asked during profile connect -- there is no D-Bus verb for "renegotiate".
// A device already connected through somebody else's endpoint (PipeWire's, on
// an ordinary desktop) will simply configure that one again, which is the
// failure this reports rather than hides.
bool BluetoothSink::negotiate(int rate, int channels) {
    endpoint_.setPreferredFormat(rate, channels);

    // Already ours, and already right: nothing to do. Re-negotiating here would
    // drop the stream at every track boundary in an album at one rate.
    if (endpoint_.hasTransport() &&
        endpoint_.transportDevice() == device_.path) {
        const a2dp::SbcCaps cur = endpoint_.configuration();
        if (a2dp::sbcFrequencyHz(cur.freq) == rate &&
            a2dp::sbcChannels(cur.channelMode) == channels) {
            cfg_ = cur;
            return true;
        }
    }

    endpoint_.disconnectProfile(device_.path);
    if (!endpoint_.connectProfile(device_.path)) {
        lastError_ = endpoint_.lastError();
        if (lastError_.empty())
            lastError_ = "could not bring up A2DP on " + device_.name;
        return false;
    }
    if (!endpoint_.waitForTransport(device_.path, kNegotiateTimeoutMs)) {
        // The single most likely reason, named, because "it did not work" is
        // what this whole backend exists to stop saying.
        lastError_ = "BlueZ gave the stream to another application (PipeWire, "
                     "most likely). Release " + device_.name + " there first.";
        return false;
    }
    cfg_ = endpoint_.configuration();
    return true;
}

bool BluetoothSink::configure(const ae::AudioFormat& want) {
    lastError_.clear();
    if (!opened_) {
        lastError_ = "no Bluetooth device is open";
        return false;
    }

    const int channels = want.channels > 0 ? want.channels : 2;
    const std::vector<int> rates = supportedRates();
    if (std::find(rates.begin(), rates.end(), want.sampleRate) == rates.end()) {
        std::string list;
        for (size_t i = 0; i < rates.size(); ++i)
            list += (i ? ", " : "") + std::to_string(rates[i]);
        lastError_ = device_.name + " accepts " + list + " Hz, not " +
                     std::to_string(want.sampleRate);
        return false;
    }

    stop();
    if (!negotiate(want.sampleRate, channels)) return false;

    if (!cfg_.isConfiguration()) {
        lastError_ = "BlueZ configured this endpoint with something that is not "
                     "a single SBC configuration";
        return false;
    }

    fmt_ = ae::AudioFormat{};
    fmt_.sampleRate   = a2dp::sbcFrequencyHz(cfg_.freq);
    fmt_.channels     = a2dp::sbcChannels(cfg_.channelMode);
    // SBC's input is S16 and nothing above can change that. Stated rather than
    // carried through from the request, so the readout names the truncation.
    fmt_.bitDepth     = 16;
    fmt_.subslotBytes = 2;
    fmt_.isFloat      = false;

    if (!encoder_.init(cfg_)) {
        lastError_ = "libsbc refused the configuration BlueZ negotiated";
        return false;
    }

    configured_ = true;
    btLog("negotiated SBC %d Hz, %d ch, bitpool %u, %zu-byte frames",
         fmt_.sampleRate, fmt_.channels, cfg_.maxBitpool, encoder_.frameLength());
    return true;
}

bool BluetoothSink::start() {
    if (!configured_) {
        lastError_ = "configure() has not run";
        return false;
    }
    if (streaming_) return true;

    if (!endpoint_.acquire(transport_)) {
        lastError_ = endpoint_.lastError();
        if (lastError_.empty()) lastError_ = "could not acquire the A2DP socket";
        return false;
    }

    // How many whole frames fit under the link's MTU, never more than the
    // payload header's four-bit frame count can express.
    const size_t overhead = a2dp::kRtpHeaderBytes + a2dp::kSbcPayloadHeaderBytes;
    size_t mtu = transport_.writeMtu;
    if (mtu <= overhead + encoder_.frameLength()) {
        // A link that cannot carry one frame plus its headers is not a link we
        // can use; better said now than as a stream of EMSGSIZE.
        if (mtu == 0) mtu = overhead + encoder_.frameLength();  // BlueZ said nothing
    }
    framesPerPacket_ = (int)((mtu - overhead) / encoder_.frameLength());
    framesPerPacket_ = std::max(1, std::min(framesPerPacket_, kMaxFramesPerPacket));

    packet_.assign(overhead + (size_t)framesPerPacket_ * encoder_.frameLength(), 0);
    packetFill_   = overhead;
    packetFrames_ = 0;

    pcmCarry_.assign(encoder_.codeSize(), 0);
    carryFill_ = 0;

    seq_ = 0;
    rtpTime_ = 0;
    resetClock();
    streaming_ = true;

    btLog("streaming to %s: MTU %u, %d frames/packet, %d ms ahead",
         device_.name.c_str(), (unsigned)transport_.writeMtu, framesPerPacket_,
         kBufferAheadMs);
    return true;
}

void BluetoothSink::resetClock() {
    clockStartUs_ = nowUs();
    framesSent_   = 0;
}

// Hand the packet under construction to the radio, then wait until we are no
// longer further ahead than the cushion allows.
bool BluetoothSink::sendPacket() {
    if (packetFrames_ <= 0) return true;

    a2dp::writeRtpHeader(packet_.data(), seq_++, rtpTime_, ssrc_);
    // The RTP-SBC payload header: fragmentation flags (all zero -- a whole
    // number of frames per packet is the point of framesPerPacket_) and the
    // frame count in the low four bits.
    packet_[a2dp::kRtpHeaderBytes] = (uint8_t)(packetFrames_ & 0x0F);

    const int samplesPerFrame =
        (int)((uint64_t)encoder_.frameDurationUs() * (uint64_t)fmt_.sampleRate /
              1000000ull);
    const int64_t framesInPacket = (int64_t)samplesPerFrame * packetFrames_;
    rtpTime_ += (uint32_t)framesInPacket;

    ssize_t wrote = ::send(transport_.fd, packet_.data(), packetFill_, MSG_NOSIGNAL);
    while (wrote < 0 && (errno == EAGAIN || errno == EINTR)) {
        // The kernel's socket buffer is full, which means the LINK is behind
        // rather than us. Waiting on the fd is the honest response; dropping
        // the packet would be a click.
        struct pollfd p{};
        p.fd = transport_.fd;
        p.events = POLLOUT;
        if (poll(&p, 1, 200) <= 0) break;
        wrote = ::send(transport_.fd, packet_.data(), packetFill_, MSG_NOSIGNAL);
    }

    packetFill_   = a2dp::kRtpHeaderBytes + a2dp::kSbcPayloadHeaderBytes;
    packetFrames_ = 0;

    if (wrote < 0) {
        // EPIPE / ECONNRESET: the headphones went. Not recoverable here, and
        // the caller learns it from write() returning -1.
        lastError_ = std::string("the A2DP link failed: ") + strerror(errno);
        btLog("%s", lastError_.c_str());
        streaming_ = false;
        return false;
    }

    framesSent_ += framesInPacket;

    // The pacing. Sleep until real time has caught up to within the cushion of
    // where the music has got to. Note this can sleep for zero -- at the start
    // of a track the whole cushion is filled as fast as the socket takes it,
    // which is what makes playback begin without a gap.
    if (fmt_.sampleRate > 0) {
        const int64_t playedUs = framesSent_ * 1000000ll / fmt_.sampleRate;
        const int64_t dueUs    = playedUs - (int64_t)kBufferAheadMs * 1000ll;
        const int64_t elapsed  = nowUs() - clockStartUs_;
        if (dueUs > elapsed)
            std::this_thread::sleep_for(std::chrono::microseconds(dueUs - elapsed));
    }
    return true;
}

int BluetoothSink::write(const uint8_t* data, int len) {
    if (!streaming_ || !data || len <= 0) return streaming_ ? 0 : -1;

    const size_t codeSize = encoder_.codeSize();
    const size_t frameLen = encoder_.frameLength();
    int consumed = 0;

    while (consumed < len) {
        // Fill one frame's worth of PCM, from the carry first.
        const size_t need = codeSize - carryFill_;
        const size_t have = (size_t)(len - consumed);
        const size_t take = std::min(need, have);
        std::memcpy(pcmCarry_.data() + carryFill_, data + consumed, take);
        carryFill_ += take;
        consumed   += (int)take;
        if (carryFill_ < codeSize) break;    // wait for the rest of the frame

        const size_t wrote =
            encoder_.encode(pcmCarry_.data(), packet_.data() + packetFill_,
                            packet_.size() - packetFill_);
        carryFill_ = 0;
        if (wrote != frameLen) {
            lastError_ = "the SBC encoder refused a frame";
            btLog("%s", lastError_.c_str());
            streaming_ = false;
            return -1;
        }
        packetFill_ += wrote;
        ++packetFrames_;

        if (packetFrames_ >= framesPerPacket_ && !sendPacket()) return -1;
    }
    return consumed;
}

// Everything not yet sent, dropped.
//
// What CAN be dropped is our own cushion -- the kernel's socket buffer and the
// radio's are not ours to clear, and there is no A2DP verb that empties them.
// kBufferAheadMs is therefore the whole worst case for a Stop, which is why it
// is 120 ms and not the 500 a lazier pacing would have allowed.
void BluetoothSink::flush() {
    if (!streaming_) return;
    carryFill_    = 0;
    packetFill_   = a2dp::kRtpHeaderBytes + a2dp::kSbcPayloadHeaderBytes;
    packetFrames_ = 0;
    // Re-anchor: the next write() must not believe it is still 120 ms ahead of
    // a track that has been thrown away, or it would sleep through the start
    // of the new one.
    resetClock();
}

int BluetoothSink::pendingPlaybackMs() const {
    if (!streaming_ || fmt_.sampleRate <= 0) return 0;
    const int64_t playedUs = framesSent_ * 1000000ll / fmt_.sampleRate;
    const int64_t elapsed  = nowUs() - clockStartUs_;
    const int64_t aheadUs  = playedUs - elapsed;
    return aheadUs <= 0 ? 0 : (int)(aheadUs / 1000);
}

void BluetoothSink::stop() {
    if (!streaming_ && transport_.fd < 0) return;
    streaming_ = false;

    if (transport_.fd >= 0) {
        ::close(transport_.fd);
        transport_.fd = -1;
    }
    // Closing the socket first and releasing after is the order BlueZ's own
    // clients use: Release() with the fd still open leaves bluetoothd waiting
    // on a descriptor nobody is reading.
    endpoint_.releaseTransport();
    transport_ = bluez::Transport{};

    carryFill_    = 0;
    packetFrames_ = 0;
    framesSent_   = 0;
}

}  // namespace ae
