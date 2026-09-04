#include "bluez_a2dp.h"

#include <systemd/sd-bus.h>

#include <cstring>

namespace ae {
namespace bluez {
namespace {

constexpr const char* kBluez        = "org.bluez";
constexpr const char* kObjManager   = "org.freedesktop.DBus.ObjectManager";
constexpr const char* kIfaceDevice  = "org.bluez.Device1";
constexpr const char* kIfaceEndpoint = "org.bluez.MediaEndpoint1";
constexpr const char* kIfaceTransport = "org.bluez.MediaTransport1";

// A2DP's own codec ids. 0xFF means "vendor", and which vendor is then another
// four bytes inside the capability blob -- that is where LDAC and aptX live.
constexpr uint8_t kCodecSbc = 0x00;

// The device is a SINK we can send to. BlueZ reports the endpoint's role from
// the endpoint's point of view, so the headphones' endpoints carry the
// AudioSink UUID and it is those we are looking for.
constexpr const char* kUuidAudioSink = "0000110b-0000-1000-8000-00805f9b34fb";

bool eqNoCase(const std::string& a, const char* b) {
    return a.size() == std::strlen(b) && strncasecmp(a.c_str(), b, a.size()) == 0;
}

// Is `child` underneath `parent` in the object tree.
bool isUnder(const std::string& child, const std::string& parent) {
    return child.size() > parent.size() + 1 &&
           child.compare(0, parent.size(), parent) == 0 &&
           child[parent.size()] == '/';
}

// One property out of an a{sv}, already positioned at the variant. Reads only
// the three types BlueZ uses for what this file wants and SKIPS anything else
// — an unknown property must not abort the walk, because BlueZ adds them
// between releases and a parser that stopped would stop working on an upgrade.
struct PropValue {
    enum class Kind { None, Str, Bool, Bytes, Byte } kind = Kind::None;
    std::string str;
    bool boolean = false;
    uint8_t byte = 0;
    std::vector<uint8_t> bytes;
};

PropValue readVariant(sd_bus_message* m) {
    PropValue v;
    const char* contents = nullptr;
    if (sd_bus_message_peek_type(m, nullptr, &contents) <= 0 || !contents) return v;

    if (std::strcmp(contents, "s") == 0 || std::strcmp(contents, "o") == 0) {
        const char* s = nullptr;
        if (sd_bus_message_read(m, "v", contents, &s) > 0 && s) {
            v.kind = PropValue::Kind::Str;
            v.str  = s;
        }
        return v;
    }
    if (std::strcmp(contents, "b") == 0) {
        int b = 0;
        if (sd_bus_message_read(m, "v", "b", &b) > 0) {
            v.kind    = PropValue::Kind::Bool;
            v.boolean = b != 0;
        }
        return v;
    }
    if (std::strcmp(contents, "y") == 0) {
        uint8_t y = 0;
        if (sd_bus_message_read(m, "v", "y", &y) > 0) {
            v.kind = PropValue::Kind::Byte;
            v.byte = y;
        }
        return v;
    }
    if (std::strcmp(contents, "ay") == 0) {
        if (sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, "ay") > 0) {
            const void* data = nullptr;
            size_t n = 0;
            if (sd_bus_message_read_array(m, SD_BUS_TYPE_BYTE, &data, &n) > 0 && data) {
                v.kind = PropValue::Kind::Bytes;
                v.bytes.assign((const uint8_t*)data, (const uint8_t*)data + n);
            }
            sd_bus_message_exit_container(m);
        }
        return v;
    }
    // Anything else: step over it whole.
    sd_bus_message_skip(m, "v");
    return v;
}

}  // namespace

Bus::Bus() = default;

Bus::~Bus() { close(); }

void Bus::setError(const char* what, int r) {
    lastError_ = std::string(what) + ": " + std::string(strerror(r < 0 ? -r : r));
}

bool Bus::open() {
    if (bus_) return true;
    // The SYSTEM bus. BlueZ lives there, not on the session bus, and this is
    // the usual first thing to get wrong.
    const int r = sd_bus_default_system(&bus_);
    if (r < 0) {
        bus_ = nullptr;
        setError("cannot reach the system bus", r);
        return false;
    }
    lastError_.clear();
    return true;
}

void Bus::close() {
    if (bus_) {
        sd_bus_unref(bus_);
        bus_ = nullptr;
    }
}

// Walks GetManagedObjects once and builds the device list from it.
//
// One call rather than a call per object: BlueZ publishes the adapters, the
// devices and every stream endpoint as one tree, and asking object by object
// would be a round trip each. It also means the endpoints are matched to their
// devices by PATH, which is what BlueZ's own layout guarantees
// (.../dev_XX/sepN) and what makes the association reliable without a second
// query.
std::vector<SinkDevice> Bus::sinks() {
    std::vector<SinkDevice> out;
    if (!bus_ && !open()) return out;

    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;
    int r = sd_bus_call_method(bus_, kBluez, "/", kObjManager,
                               "GetManagedObjects", &err, &reply, "");
    if (r < 0) {
        lastError_ = err.message ? err.message : "BlueZ is not running";
        sd_bus_error_free(&err);
        return out;
    }

    // Endpoints are collected alongside the devices and attached afterwards:
    // GetManagedObjects makes no ordering promise, so an endpoint can arrive
    // before the device it belongs to.
    struct Endpoint {
        std::string path, uuid;
        uint8_t codec = 0;
        std::vector<uint8_t> caps;
    };
    std::vector<Endpoint> endpoints;

    if (sd_bus_message_enter_container(reply, SD_BUS_TYPE_ARRAY, "{oa{sa{sv}}}") > 0) {
        while (sd_bus_message_enter_container(reply, SD_BUS_TYPE_DICT_ENTRY, "oa{sa{sv}}") > 0) {
            const char* objPath = nullptr;
            sd_bus_message_read_basic(reply, SD_BUS_TYPE_OBJECT_PATH, &objPath);
            const std::string path = objPath ? objPath : "";

            SinkDevice dev;
            Endpoint   ep;
            bool isDevice = false, isEndpoint = false;

            if (sd_bus_message_enter_container(reply, SD_BUS_TYPE_ARRAY, "{sa{sv}}") > 0) {
                while (sd_bus_message_enter_container(reply, SD_BUS_TYPE_DICT_ENTRY, "sa{sv}") > 0) {
                    const char* iface = nullptr;
                    sd_bus_message_read_basic(reply, SD_BUS_TYPE_STRING, &iface);
                    const std::string ifaceName = iface ? iface : "";
                    isDevice   |= (ifaceName == kIfaceDevice);
                    isEndpoint |= (ifaceName == kIfaceEndpoint);

                    if (sd_bus_message_enter_container(reply, SD_BUS_TYPE_ARRAY, "{sv}") > 0) {
                        while (sd_bus_message_enter_container(reply, SD_BUS_TYPE_DICT_ENTRY, "sv") > 0) {
                            const char* prop = nullptr;
                            sd_bus_message_read_basic(reply, SD_BUS_TYPE_STRING, &prop);
                            const std::string propName = prop ? prop : "";
                            const PropValue v = readVariant(reply);

                            if (ifaceName == kIfaceDevice) {
                                if (propName == "Address"   && v.kind == PropValue::Kind::Str)  dev.address = v.str;
                                else if (propName == "Alias" && v.kind == PropValue::Kind::Str) dev.name = v.str;
                                else if (propName == "Name"  && v.kind == PropValue::Kind::Str && dev.name.empty()) dev.name = v.str;
                                else if (propName == "Adapter" && v.kind == PropValue::Kind::Str) dev.adapter = v.str;
                                else if (propName == "Connected" && v.kind == PropValue::Kind::Bool) dev.connected = v.boolean;
                            } else if (ifaceName == kIfaceEndpoint) {
                                if (propName == "UUID"  && v.kind == PropValue::Kind::Str)   ep.uuid = v.str;
                                else if (propName == "Codec" && v.kind == PropValue::Kind::Byte) ep.codec = v.byte;
                                else if (propName == "Capabilities" && v.kind == PropValue::Kind::Bytes) ep.caps = v.bytes;
                            }
                            sd_bus_message_exit_container(reply);   // sv
                        }
                        sd_bus_message_exit_container(reply);       // a{sv}
                    }
                    sd_bus_message_exit_container(reply);           // sa{sv}
                }
                sd_bus_message_exit_container(reply);               // a{sa{sv}}
            }
            sd_bus_message_exit_container(reply);                   // oa{sa{sv}}

            if (isDevice) {
                dev.path = path;
                out.push_back(std::move(dev));
            } else if (isEndpoint) {
                ep.path = path;
                endpoints.push_back(std::move(ep));
            }
        }
        sd_bus_message_exit_container(reply);                       // the array
    }

    sd_bus_message_unref(reply);
    sd_bus_error_free(&err);

    // Attach each endpoint to the device whose path it sits under.
    for (const Endpoint& ep : endpoints) {
        // Only endpoints the REMOTE side offers as a sink are ours to feed.
        if (!eqNoCase(ep.uuid, kUuidAudioSink)) continue;
        for (SinkDevice& dev : out) {
            if (!isUnder(ep.path, dev.path)) continue;
            dev.codecIds.push_back(ep.codec);
            if (ep.codec == kCodecSbc && ep.caps.size() >= a2dp::kSbcCapsBytes) {
                dev.sbcEndpointPath = ep.path;
                dev.sbcCaps = a2dp::parseSbcCaps(ep.caps.data(), ep.caps.size());
                dev.hasSbc  = true;
            }
            break;
        }
    }

    // A device with no audio endpoint at all is not an A2DP sink -- it is a
    // mouse, a keyboard, a phone. Dropped here rather than shown and refused.
    std::vector<SinkDevice> audioOnly;
    for (SinkDevice& d : out)
        if (!d.codecIds.empty()) audioOnly.push_back(std::move(d));
    return audioOnly;
}

TransportState Bus::transportFor(const std::string& devicePath) {
    TransportState st;
    if (devicePath.empty()) return st;
    if (!bus_ && !open()) return st;

    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;
    int r = sd_bus_call_method(bus_, kBluez, "/", kObjManager,
                               "GetManagedObjects", &err, &reply, "");
    if (r < 0) {
        lastError_ = err.message ? err.message : "BlueZ is not running";
        sd_bus_error_free(&err);
        return st;
    }

    if (sd_bus_message_enter_container(reply, SD_BUS_TYPE_ARRAY, "{oa{sa{sv}}}") > 0) {
        while (sd_bus_message_enter_container(reply, SD_BUS_TYPE_DICT_ENTRY, "oa{sa{sv}}") > 0) {
            const char* objPath = nullptr;
            sd_bus_message_read_basic(reply, SD_BUS_TYPE_OBJECT_PATH, &objPath);
            const std::string path = objPath ? objPath : "";

            bool isTransport = false;
            std::string state;
            uint8_t codec = 0;

            if (sd_bus_message_enter_container(reply, SD_BUS_TYPE_ARRAY, "{sa{sv}}") > 0) {
                while (sd_bus_message_enter_container(reply, SD_BUS_TYPE_DICT_ENTRY, "sa{sv}") > 0) {
                    const char* iface = nullptr;
                    sd_bus_message_read_basic(reply, SD_BUS_TYPE_STRING, &iface);
                    const std::string ifaceName = iface ? iface : "";
                    isTransport |= (ifaceName == kIfaceTransport);

                    if (sd_bus_message_enter_container(reply, SD_BUS_TYPE_ARRAY, "{sv}") > 0) {
                        while (sd_bus_message_enter_container(reply, SD_BUS_TYPE_DICT_ENTRY, "sv") > 0) {
                            const char* prop = nullptr;
                            sd_bus_message_read_basic(reply, SD_BUS_TYPE_STRING, &prop);
                            const std::string propName = prop ? prop : "";
                            const PropValue v = readVariant(reply);
                            if (ifaceName == kIfaceTransport) {
                                if (propName == "State" && v.kind == PropValue::Kind::Str) state = v.str;
                                else if (propName == "Codec" && v.kind == PropValue::Kind::Byte) codec = v.byte;
                            }
                            sd_bus_message_exit_container(reply);
                        }
                        sd_bus_message_exit_container(reply);
                    }
                    sd_bus_message_exit_container(reply);
                }
                sd_bus_message_exit_container(reply);
            }
            sd_bus_message_exit_container(reply);

            if (isTransport && isUnder(path, devicePath)) {
                st.exists  = true;
                st.path    = path;
                st.state   = state;
                st.codecId = codec;
            }
        }
        sd_bus_message_exit_container(reply);
    }

    sd_bus_message_unref(reply);
    sd_bus_error_free(&err);
    return st;
}

}  // namespace bluez
}  // namespace ae
