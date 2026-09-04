#ifndef AE_BACKENDS_BLUETOOTH_BLUEZ_A2DP_H
#define AE_BACKENDS_BLUETOOTH_BLUEZ_A2DP_H

// BlueZ, over D-Bus, from the A2DP SOURCE side.
//
// This is the half of the Bluetooth backend that talks to the system: it finds
// the adapters, finds the connected sinks, and reads what each of them says it
// can decode. Encoding and framing are a2dp_sbc.h's job and nothing here knows
// about either.
//
// --- why this exists at all, rather than asking PipeWire ---------------------
//
// PipeWire already implements A2DP and can switch codecs, and driving it would
// have been a fraction of this work. It was rejected for the same reason the
// USB backend bypasses the OS mixer: asking a sound server to pick a codec
// means the samples reach the radio through its resampler, its mixer and its
// volume stage, and this project's whole claim is that nothing touches them.
// Registering our own endpoint means the bytes we encode are the bytes that go
// out.
//
// --- the consequence, which is not hidden ------------------------------------
//
// A transport belongs to exactly ONE source endpoint. If PipeWire is holding
// the device -- and on a normal desktop it is, from the moment the headphones
// connect -- then we cannot have it, and the honest report is that the device
// is busy rather than a stream that silently goes nowhere. This is the same
// shape as PipeWire holding an ALSA card, which this project already handles by
// putting the driver's own words on screen.
//
// Uses sd-bus rather than libdbus: BlueZ's interface is property-heavy and
// sd-bus reads a variant dictionary in a fraction of the code. The whole
// backend is gated on it being found (see CMakeLists.txt), exactly as ALSA and
// JACK are.
#include <cstdint>
#include <string>
#include <vector>

#include "a2dp_sbc.h"

// Forward-declared at GLOBAL scope on purpose. Written inside the namespace
// below it would declare ae::bluez::sd_bus -- a different, incomplete type
// from sd-bus's own ::sd_bus -- and every call would fail to convert.
struct sd_bus;

namespace ae {
namespace bluez {

// One connected A2DP sink, as BlueZ describes it.
struct SinkDevice {
    std::string path;        // /org/bluez/hci0/dev_XX_XX_...
    std::string adapter;     // /org/bluez/hci0
    std::string address;     // XX:XX:XX:XX:XX:XX
    std::string name;        // the Alias, which is what a listener recognises
    bool connected = false;

    // The device's SBC stream endpoint, when it has one. Every A2DP sink is
    // required to, so an empty path here is worth reporting rather than
    // ignoring -- it means BlueZ has not finished discovering the endpoints,
    // which happens for a second or two after a connection.
    std::string sbcEndpointPath;
    a2dp::SbcCaps sbcCaps;
    bool hasSbc = false;

    // Every codec the device advertises, by A2DP codec id (0 = SBC, 2 = AAC,
    // 0xFF = vendor, which is where LDAC and aptX live). Reported so the
    // settings panel can say what a device could do, not only what we can
    // currently give it.
    std::vector<uint8_t> codecIds;
};

// Whether anything else already owns a device's transport. There is no D-Bus
// call that says "PipeWire has this"; what CAN be seen is a MediaTransport1
// object already existing under the device, which is what a live stream looks
// like from outside.
struct TransportState {
    bool exists = false;     // a MediaTransport1 is present under the device
    std::string path;
    std::string state;       // "idle" | "pending" | "active"
    uint8_t codecId = 0;
};

// A live connection to the system bus and BlueZ. Cheap to construct; every
// method is a blocking D-Bus round trip, so none of them belongs on an audio
// thread.
class Bus {
public:
    Bus();
    ~Bus();

    Bus(const Bus&) = delete;
    Bus& operator=(const Bus&) = delete;

    // False when there is no system bus, or BlueZ is not running. lastError()
    // carries D-Bus's own words, which is what reaches the screen.
    bool open();
    void close();
    bool isOpen() const { return bus_ != nullptr; }

    const std::string& lastError() const { return lastError_; }

    // Every A2DP sink BlueZ currently knows about, connected or not.
    std::vector<SinkDevice> sinks();

    // Is somebody already streaming to this device.
    TransportState transportFor(const std::string& devicePath);

private:
    ::sd_bus* bus_ = nullptr;
    std::string lastError_;

    void setError(const char* what, int r);
};

}  // namespace bluez
}  // namespace ae

#endif  // AE_BACKENDS_BLUETOOTH_BLUEZ_A2DP_H
