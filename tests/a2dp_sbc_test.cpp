// A2DP SBC capability negotiation and RTP framing.
//
// The capability bytes below are NOT invented: they were read off real
// hardware over D-Bus (an LG-PL7 speaker, org.bluez .../sep1, Codec 0), which
// is the whole point — this is a wire format negotiated with a device across a
// radio link, and a test against made-up bytes would only prove the code
// agrees with itself.
//
// Runs on the desktop with no Bluetooth, no adapter and no device: every
// function under test is pure.
#include "backends/bluetooth/a2dp_sbc.h"

#include <cstdio>
#include <cstring>

using namespace ae::a2dp;

static int failures = 0;
static void check(bool cond, const char* what) {
    if (!cond) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

// ── 1. The real device's capabilities parse to what busctl showed ───────────
static void testParseRealDevice() {
    // busctl --system introspect org.bluez .../sep1 org.bluez.MediaEndpoint1
    //   .Capabilities  ay  4  255 255 2 53
    const uint8_t lgPl7[] = { 0xFF, 0xFF, 0x02, 0x35 };
    const SbcCaps c = parseSbcCaps(lgPl7, sizeof(lgPl7));

    check(c.freq == (kFreq16000 | kFreq32000 | kFreq44100 | kFreq48000),
          "LG-PL7 offers all four sample rates");
    check(c.channelMode == (kChanMono | kChanDual | kChanStereo | kChanJointStereo),
          "LG-PL7 offers all four channel modes");
    check(c.blockLength == (kBlocks4 | kBlocks8 | kBlocks12 | kBlocks16),
          "LG-PL7 offers all four block lengths");
    check(c.subbands == (kSubbands4 | kSubbands8), "LG-PL7 offers 4 and 8 subbands");
    check(c.allocation == (kAllocSnr | kAllocLoudness), "LG-PL7 offers both allocations");
    check(c.minBitpool == 2,  "LG-PL7 min bitpool is 2");
    check(c.maxBitpool == 53, "LG-PL7 max bitpool is 53");
    check(!c.isConfiguration(), "a capability with several bits set is not a configuration");
}

// ── 2. Round-tripping the four bytes ────────────────────────────────────────
static void testRoundTrip() {
    const uint8_t original[] = { 0xFF, 0xFF, 0x02, 0x35 };
    SbcCaps c = parseSbcCaps(original, sizeof(original));
    uint8_t out[4] = {0, 0, 0, 0};
    writeSbcCaps(c, out);
    check(std::memcmp(original, out, 4) == 0,
          "capability bytes survive parse -> write unchanged");

    // A short or absent blob must not read past the end.
    const uint8_t truncated[] = { 0xFF };
    SbcCaps t = parseSbcCaps(truncated, sizeof(truncated));
    check(t.freq == 0 && t.maxBitpool == 0, "a truncated capability parses to nothing");
    SbcCaps n = parseSbcCaps(nullptr, 0);
    check(n.freq == 0, "a null capability parses to nothing");
}

// ── 3. What we would actually choose for this speaker ───────────────────────
static void testSelectAgainstRealDevice() {
    const uint8_t lgPl7[] = { 0xFF, 0xFF, 0x02, 0x35 };
    const SbcCaps caps = parseSbcCaps(lgPl7, sizeof(lgPl7));

    SbcCaps cfg;
    check(selectSbcConfiguration(caps, 44100, 2, cfg), "a fully-capable sink negotiates");
    check(cfg.isConfiguration(), "the result is a CONFIGURATION: one bit per field");
    check(cfg.freq == kFreq44100, "44.1 kHz source keeps 44.1 kHz -- never resampled");
    check(cfg.channelMode == kChanJointStereo, "joint stereo is preferred over plain stereo");
    check(cfg.blockLength == kBlocks16, "the longest block length is chosen");
    check(cfg.subbands == kSubbands8, "8 subbands, the finer analysis");
    check(cfg.allocation == kAllocLoudness, "loudness allocation, as A2DP recommends");
    check(cfg.maxBitpool == 53, "the sink's own maximum bitpool is taken, not lowered");

    // A 48 kHz source on the same speaker stays at 48 kHz.
    SbcCaps cfg48;
    check(selectSbcConfiguration(caps, 48000, 2, cfg48), "48 kHz negotiates");
    check(cfg48.freq == kFreq48000, "48 kHz source keeps 48 kHz");

    // A rate the sink cannot take falls back to its best, rather than failing:
    // 96 kHz is not an A2DP SBC rate at all, and the alternative to falling
    // back is no sound.
    SbcCaps cfg96;
    check(selectSbcConfiguration(caps, 96000, 2, cfg96), "an impossible rate still negotiates");
    check(cfg96.freq == kFreq48000, "...by falling back to the sink's highest");
}

// ── 4. Sinks that do NOT offer everything ───────────────────────────────────
static void testConstrainedSinks() {
    // 44.1 kHz only, stereo only, 8 blocks only, 4 subbands, SNR only.
    SbcCaps poor;
    poor.freq        = kFreq44100;
    poor.channelMode = kChanStereo;
    poor.blockLength = kBlocks8;
    poor.subbands    = kSubbands4;
    poor.allocation  = kAllocSnr;
    poor.minBitpool  = 2;
    poor.maxBitpool  = 35;

    SbcCaps cfg;
    check(selectSbcConfiguration(poor, 48000, 2, cfg), "a constrained sink still negotiates");
    check(cfg.freq == kFreq44100, "a 48 kHz source falls back to the only rate offered");
    check(cfg.channelMode == kChanStereo, "plain stereo when joint is not offered");
    check(cfg.allocation == kAllocSnr, "SNR when loudness is not offered");
    check(cfg.maxBitpool == 35, "the sink's lower bitpool ceiling is respected");

    // No overlap at all is refused rather than answered with nonsense.
    SbcCaps none;
    none.freq = 0;
    SbcCaps unused;
    check(!selectSbcConfiguration(none, 44100, 2, unused),
          "a sink offering no sample rate is refused, not guessed at");

    // A device advertising a bitpool past the format's ceiling is clamped: a
    // frame built at 255 would simply be dropped by the sink.
    SbcCaps liar = poor;
    liar.maxBitpool = 255;
    SbcCaps clamped;
    check(selectSbcConfiguration(liar, 44100, 2, clamped), "an over-large bitpool negotiates");
    check(clamped.maxBitpool == 250, "...clamped to SBC's own maximum of 250");
}

// ── 5. Mono sources ─────────────────────────────────────────────────────────
static void testMono() {
    const uint8_t lgPl7[] = { 0xFF, 0xFF, 0x02, 0x35 };
    const SbcCaps caps = parseSbcCaps(lgPl7, sizeof(lgPl7));
    SbcCaps cfg;
    check(selectSbcConfiguration(caps, 44100, 1, cfg), "a mono source negotiates");
    check(cfg.channelMode == kChanMono, "a mono source asks for mono");
    check(sbcChannels(cfg.channelMode) == 1, "and reads back as one channel");
}

// ── 6. The frequency/channel readbacks ──────────────────────────────────────
static void testReadbacks() {
    check(sbcFrequencyHz(kFreq44100) == 44100, "44.1 kHz bit reads back");
    check(sbcFrequencyHz(kFreq48000) == 48000, "48 kHz bit reads back");
    check(sbcFrequencyHz(0) == 0, "no bit reads back as unknown");
    check(sbcFrequencyHz(kFreq44100 | kFreq48000) == 0,
          "two bits is not a configuration and reads back as unknown");
    check(sbcChannels(kChanJointStereo) == 2, "joint stereo is two channels");
    check(sbcChannels(kChanMono) == 1, "mono is one channel");
}

// ── 7. The RTP header ───────────────────────────────────────────────────────
static void testRtpHeader() {
    uint8_t h[kRtpHeaderBytes];
    std::memset(h, 0xEE, sizeof(h));
    writeRtpHeader(h, 0x1234, 0xDEADBEEF, 0xCAFEF00D);

    check(h[0] == 0x80, "RTP version 2, no padding, no extension, no CSRCs");
    check(h[1] == 96,   "payload type 96, the dynamic type A2DP uses");
    // Network order, which is the one thing here a little-endian machine gets
    // wrong by default.
    check(h[2] == 0x12 && h[3] == 0x34, "sequence number is big-endian");
    check(h[4] == 0xDE && h[5] == 0xAD && h[6] == 0xBE && h[7] == 0xEF,
          "timestamp is big-endian");
    check(h[8] == 0xCA && h[9] == 0xFE && h[10] == 0xF0 && h[11] == 0x0D,
          "SSRC is big-endian");

    // Sequence numbers wrap, and the wrap must not disturb anything else.
    writeRtpHeader(h, 0xFFFF, 0, 0);
    check(h[2] == 0xFF && h[3] == 0xFF, "a sequence number at its maximum writes out");
    writeRtpHeader(h, 0x0000, 0, 0);
    check(h[2] == 0x00 && h[3] == 0x00, "and wraps to zero cleanly");
}

int main() {
    testParseRealDevice();
    testRoundTrip();
    testSelectAgainstRealDevice();
    testConstrainedSinks();
    testMono();
    testReadbacks();
    testRtpHeader();
    if (failures == 0) std::puts("a2dp_sbc_test: all checks passed");
    return failures == 0 ? 0 : 1;
}
