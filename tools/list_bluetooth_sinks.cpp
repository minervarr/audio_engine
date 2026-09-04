// What BlueZ says about the A2DP sinks on this machine, and what we would
// negotiate with each of them.
//
// The sibling of list_usb_devices / list_alsa_devices: a bring-up tool, not
// part of any app. It answers the questions that can only be answered against
// a real adapter and a real pair of headphones — is the device visible, does
// it publish an SBC endpoint, what did it say it can decode, what would we
// choose out of that, and is something else already streaming to it.
//
//     ./list_bluetooth_sinks
//
// Cross-check the raw numbers against BlueZ itself with:
//
//     busctl --system introspect org.bluez \ 
//         /org/bluez/hci0/dev_XX/sepN org.bluez.MediaEndpoint1
#include <cstdio>
#include <string>

#include "backends/bluetooth/a2dp_sbc.h"
#include "backends/bluetooth/bluez_a2dp.h"

using namespace ae;

static std::string codecName(uint8_t id) {
    switch (id) {
    case 0x00: return "SBC";
    case 0x01: return "MPEG-1,2 Audio";
    case 0x02: return "MPEG-2,4 AAC";
    case 0x04: return "ATRAC";
    case 0xFF: return "vendor (LDAC / aptX / ...)";
    default:   return "codec " + std::to_string(id);
    }
}

static void printSbc(const a2dp::SbcCaps& c, const char* indent) {
    std::printf("%sfrequencies :%s%s%s%s\n", indent,
                (c.freq & a2dp::kFreq16000) ? " 16k"   : "",
                (c.freq & a2dp::kFreq32000) ? " 32k"   : "",
                (c.freq & a2dp::kFreq44100) ? " 44.1k" : "",
                (c.freq & a2dp::kFreq48000) ? " 48k"   : "");
    std::printf("%schannels    :%s%s%s%s\n", indent,
                (c.channelMode & a2dp::kChanMono)        ? " mono"   : "",
                (c.channelMode & a2dp::kChanDual)        ? " dual"   : "",
                (c.channelMode & a2dp::kChanStereo)      ? " stereo" : "",
                (c.channelMode & a2dp::kChanJointStereo) ? " joint"  : "");
    std::printf("%sblocks      :%s%s%s%s\n", indent,
                (c.blockLength & a2dp::kBlocks4)  ? " 4"  : "",
                (c.blockLength & a2dp::kBlocks8)  ? " 8"  : "",
                (c.blockLength & a2dp::kBlocks12) ? " 12" : "",
                (c.blockLength & a2dp::kBlocks16) ? " 16" : "");
    std::printf("%ssubbands    :%s%s\n", indent,
                (c.subbands & a2dp::kSubbands4) ? " 4" : "",
                (c.subbands & a2dp::kSubbands8) ? " 8" : "");
    std::printf("%sallocation  :%s%s\n", indent,
                (c.allocation & a2dp::kAllocSnr)      ? " SNR"      : "",
                (c.allocation & a2dp::kAllocLoudness) ? " loudness" : "");
    std::printf("%sbitpool     : %u..%u\n", indent, c.minBitpool, c.maxBitpool);
}

int main() {
    bluez::Bus bus;
    if (!bus.open()) {
        std::printf("BlueZ unreachable: %s\n", bus.lastError().c_str());
        return 1;
    }

    const std::vector<bluez::SinkDevice> sinks = bus.sinks();
    if (sinks.empty()) {
        std::printf("No A2DP audio devices known to BlueZ.\n");
        if (!bus.lastError().empty())
            std::printf("  (%s)\n", bus.lastError().c_str());
        return 0;
    }

    for (const bluez::SinkDevice& d : sinks) {
        std::printf("\n%s  [%s]%s\n", d.name.c_str(), d.address.c_str(),
                    d.connected ? "" : "  (not connected)");
        std::printf("  path      : %s\n", d.path.c_str());
        std::printf("  adapter   : %s\n", d.adapter.c_str());

        std::printf("  advertises:");
        for (uint8_t id : d.codecIds) std::printf(" %s;", codecName(id).c_str());
        std::printf("\n");

        if (!d.hasSbc) {
            // Every A2DP sink must implement SBC, so this is BlueZ still
            // discovering the endpoints rather than a device that lacks it.
            std::printf("  SBC       : no endpoint published yet\n");
        } else {
            std::printf("  SBC endpoint %s says it can take:\n", d.sbcEndpointPath.c_str());
            printSbc(d.sbcCaps, "    ");

            a2dp::SbcCaps chosen;
            if (a2dp::selectSbcConfiguration(d.sbcCaps, 44100, 2, chosen)) {
                std::printf("  we would negotiate (44.1k stereo source):\n");
                std::printf("    %d Hz, %d ch, %s, %s blocks, %s subbands, %s, bitpool %u\n",
                            a2dp::sbcFrequencyHz(chosen.freq),
                            a2dp::sbcChannels(chosen.channelMode),
                            chosen.channelMode == a2dp::kChanJointStereo ? "joint stereo"
                                                                         : "stereo",
                            chosen.blockLength == a2dp::kBlocks16 ? "16" : "fewer",
                            chosen.subbands == a2dp::kSubbands8 ? "8" : "4",
                            chosen.allocation == a2dp::kAllocLoudness ? "loudness" : "SNR",
                            chosen.maxBitpool);
            } else {
                std::printf("  we could NOT negotiate anything with this sink\n");
            }
        }

        const bluez::TransportState t = bus.transportFor(d.path);
        if (t.exists) {
            // The conflict, named. A transport belongs to one source endpoint,
            // so while this is here the device is not ours to take.
            std::printf("  IN USE    : %s is %s, carrying %s\n",
                        t.path.c_str(), t.state.c_str(), codecName(t.codecId).c_str());
            std::printf("              something else (PipeWire, most likely) owns this device.\n");
        } else {
            std::printf("  free      : no transport; the device is available\n");
        }
    }
    std::printf("\n");
    return 0;
}
