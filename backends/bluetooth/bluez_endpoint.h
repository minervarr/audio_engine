#ifndef AE_BACKENDS_BLUETOOTH_BLUEZ_ENDPOINT_H
#define AE_BACKENDS_BLUETOOTH_BLUEZ_ENDPOINT_H

// OUR OWN org.bluez.MediaEndpoint1, registered on an adapter as an A2DP
// SOURCE.
//
// bluez_a2dp.h reads what BlueZ knows. This file makes us one of the things it
// knows: an object on the system bus that BlueZ calls back to ask "here is what
// the headphones can decode, what will you send?" and then "here is the socket".
// Owning that object is what owns the stream -- the bytes that leave this
// process are the bytes that reach the radio, with no sound server's resampler,
// mixer or volume stage between.
//
// --- the one transport rule -------------------------------------------------
//
// A device carries ONE MediaTransport1 at a time and its owner is whoever
// registered the endpoint BlueZ configured. That has three consequences, all of
// them stated rather than worked around:
//
//   * If PipeWire is holding the device -- the normal state of a desktop the
//     moment headphones connect -- BlueZ configured PIPEWIRE'S endpoint, and
//     acquire() will be refused. The honest report is "something else is
//     streaming to this device", not silence.
//   * Acquire() must be called on the SAME D-Bus connection that registered the
//     endpoint. BlueZ authorises the transport by the registrant's unique name,
//     so a second connection is rejected with NotAuthorized. This is why every
//     call here goes through one connection and one mutex rather than the
//     simpler two-connection arrangement.
//   * BlueZ decides WHEN to configure, so SetConfiguration arrives on the bus
//     thread at a moment we do not choose. waitForTransport() is how the caller
//     joins that back up.
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "a2dp_sbc.h"

struct sd_bus;              // see bluez_a2dp.h: global scope on purpose
struct sd_bus_message;

namespace ae {
namespace bluez {

// The A2DP SOURCE profile. We register as a source; the headphones are the
// sink, and it is their AudioSink UUID bluez_a2dp.cpp filters endpoints on.
constexpr const char* kUuidA2dpSource = "0000110a-0000-1000-8000-00805f9b34fb";
constexpr const char* kUuidA2dpSink   = "0000110b-0000-1000-8000-00805f9b34fb";

// The live socket to one device, as MediaTransport1.Acquire() hands it over.
struct Transport {
    int      fd      = -1;   // an L2CAP seqpacket socket; OWNED by the caller
    uint16_t writeMtu = 0;   // the largest packet the link will carry
    uint16_t readMtu  = 0;
};

class Endpoint {
public:
    Endpoint();
    ~Endpoint();

    Endpoint(const Endpoint&) = delete;
    Endpoint& operator=(const Endpoint&) = delete;

    // Open a private connection, publish the endpoint object, hand it to the
    // adapter, and start pumping the bus. `ourCaps` is what we tell BlueZ we
    // can PRODUCE -- a capability blob, several bits per field; the single
    // configuration is chosen later, per device, inside SelectConfiguration.
    bool start(const std::string& adapterPath, const a2dp::SbcCaps& ourCaps);

    // What SelectConfiguration should aim for when BlueZ next asks. It asks at
    // CONNECT time, long before a track is chosen, so the rate has to be told
    // ahead rather than passed in -- and telling it means disconnecting and
    // reconnecting the profile, which is what BluetoothSink::configure() does
    // when the source rate changes. 0 leaves the default (44.1 kHz stereo).
    void setPreferredFormat(int rate, int channels);
    void stop();
    bool running() const { return running_.load(std::memory_order_acquire); }

    const std::string& lastError() const;

    // Ask BlueZ to bring the A2DP profile up on a device, which is what makes
    // it choose an endpoint and call SetConfiguration. A device already
    // connected through somebody else's endpoint will NOT hand its transport
    // over -- see the one transport rule above.
    bool connectProfile(const std::string& devicePath);
    bool disconnectProfile(const std::string& devicePath);

    // Block until BlueZ has configured US for `devicePath` (empty = any
    // device), or the timeout runs out. False means nobody called
    // SetConfiguration, which usually means the device is somebody else's.
    bool waitForTransport(const std::string& devicePath, int timeoutMs);

    // What SetConfiguration was told, once it has arrived.
    bool        hasTransport() const;
    std::string transportPath() const;
    std::string transportDevice() const;
    a2dp::SbcCaps configuration() const;

    // Take the socket. The fd belongs to the caller from here on: close it and
    // then call releaseTransport(), in that order, exactly as BlueZ's own
    // clients do.
    bool acquire(Transport& out);
    void releaseTransport();

private:
public:
    // The sd-bus vtable callbacks. Static because sd-bus is C; each recovers
    // `this` from the userdata pointer given to sd_bus_add_object_vtable. The
    // error argument is a void* so this header needs no <systemd/sd-bus.h>;
    // four one-line trampolines in the .cpp give sd-bus the type it wants.
    // Public only because those trampolines are free functions -- nothing
    // outside the implementation has any reason to call them.
    static int selectConfigurationThunk(sd_bus_message* m, void* user, void* err);
    static int setConfigurationThunk(sd_bus_message* m, void* user, void* err);
    static int clearConfigurationThunk(sd_bus_message* m, void* user, void* err);
    static int releaseThunk(sd_bus_message* m, void* user, void* err);

private:

    void loop();
    void setError(const char* what, int r);

    ::sd_bus* bus_ = nullptr;

    // ONE lock over the connection. Every sd_bus_* call in this file takes it,
    // including the loop's own sd_bus_process; the loop drops it to poll(), so
    // an Acquire from the audio side never waits on the bus being idle.
    mutable std::mutex      busMu_;
    std::thread             thread_;
    std::atomic<bool>       running_{false};

    mutable std::mutex      stateMu_;
    std::condition_variable stateCv_;
    std::string             lastError_;
    a2dp::SbcCaps           ourCaps_{};
    int                     wantRate_ = 44100;
    int                     wantChannels_ = 2;
    a2dp::SbcCaps           chosen_{};
    std::string             transportPath_;
    std::string             transportDevice_;
    bool                    haveTransport_ = false;
    bool                    acquired_ = false;
};

}  // namespace bluez
}  // namespace ae

#endif  // AE_BACKENDS_BLUETOOTH_BLUEZ_ENDPOINT_H
