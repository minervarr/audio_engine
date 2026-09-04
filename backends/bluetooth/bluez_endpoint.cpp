#include "bluez_endpoint.h"

#include <poll.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>

namespace ae {
namespace bluez {
namespace {

constexpr const char* kBluez         = "org.bluez";
constexpr const char* kIfaceMedia    = "org.bluez.Media1";
constexpr const char* kIfaceEndpoint = "org.bluez.MediaEndpoint1";
constexpr const char* kIfaceTransport = "org.bluez.MediaTransport1";
constexpr const char* kIfaceDevice   = "org.bluez.Device1";

// Where our object lives on the bus. One per process is enough: an endpoint is
// per ADAPTER and a machine with two Bluetooth radios both carrying music is
// not a case this player has.
constexpr const char* kObjectPath = "/io/nava/matrix_player/a2dp/sbc";

// How long the bus thread sleeps with nothing to do. Only an upper bound --
// poll() returns the moment BlueZ says anything -- so it costs nothing to keep
// short, and short is what makes stop() prompt.
constexpr int kPollSliceMs = 100;

}  // namespace

// --- the endpoint object ----------------------------------------------------

// BlueZ asks this ONCE per device, with that device's capability blob, and the
// answer is the configuration the stream will run at for as long as it lasts.
// Getting it wrong is not a crash: it is a device that refuses the stream, or
// takes it and plays it worse than it could have.
int Endpoint::selectConfigurationThunk(sd_bus_message* m, void* user, void* err) {
    Endpoint* self = (Endpoint*)user;
    const void* data = nullptr;
    size_t n = 0;
    int r = sd_bus_message_read_array(m, SD_BUS_TYPE_BYTE, &data, &n);
    if (r < 0 || !data || n < a2dp::kSbcCapsBytes) {
        sd_bus_error_set_const((sd_bus_error*)err,
                               "org.bluez.Error.InvalidArguments",
                               "capabilities too short for SBC");
        return -EINVAL;
    }

    const a2dp::SbcCaps remote =
        a2dp::parseSbcCaps((const uint8_t*)data, n);

    // What we ASK for, not what we impose: the source rate is not known here
    // (BlueZ configures on connect, long before a track is chosen), so it is
    // whatever setPreferredFormat() was last told -- 44.1 kHz stereo until
    // somebody knows better, that being the rate most of a music library is at
    // and therefore the choice that resamples least.
    // selectSbcConfiguration() falls back to the highest rate the sink offers
    // when it cannot have that one.
    int wantRate, wantChannels;
    {
        std::lock_guard<std::mutex> lk(self->stateMu_);
        wantRate     = self->wantRate_;
        wantChannels = self->wantChannels_;
    }

    a2dp::SbcCaps chosen;
    if (!a2dp::selectSbcConfiguration(remote, wantRate, wantChannels, chosen)) {
        sd_bus_error_set_const((sd_bus_error*)err,
                               "org.bluez.Error.InvalidArguments",
                               "no SBC configuration in common with this device");
        return -EINVAL;
    }

    {
        std::lock_guard<std::mutex> lk(self->stateMu_);
        self->chosen_ = chosen;
    }

    uint8_t wire[a2dp::kSbcCapsBytes];
    a2dp::writeSbcCaps(chosen, wire);

    sd_bus_message* reply = nullptr;
    r = sd_bus_message_new_method_return(m, &reply);
    if (r < 0) return r;
    r = sd_bus_message_append_array(reply, SD_BUS_TYPE_BYTE, wire, sizeof(wire));
    if (r < 0) { sd_bus_message_unref(reply); return r; }
    r = sd_bus_send(nullptr, reply, nullptr);
    sd_bus_message_unref(reply);
    return r < 0 ? r : 1;
}

// BlueZ has decided: this transport is ours. It arrives on the bus thread at a
// moment nobody chose, which is what waitForTransport() exists to join back up.
int Endpoint::setConfigurationThunk(sd_bus_message* m, void* user, void* err) {
    (void)err;
    Endpoint* self = (Endpoint*)user;

    const char* transport = nullptr;
    if (sd_bus_message_read_basic(m, SD_BUS_TYPE_OBJECT_PATH, &transport) < 0)
        return -EINVAL;

    std::string device;
    a2dp::SbcCaps cfg;
    bool haveCfg = false;

    if (sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "{sv}") > 0) {
        while (sd_bus_message_enter_container(m, SD_BUS_TYPE_DICT_ENTRY, "sv") > 0) {
            const char* prop = nullptr;
            sd_bus_message_read_basic(m, SD_BUS_TYPE_STRING, &prop);
            const std::string name = prop ? prop : "";

            if (name == "Device") {
                const char* o = nullptr;
                if (sd_bus_message_read(m, "v", "o", &o) > 0 && o) device = o;
            } else if (name == "Configuration") {
                if (sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, "ay") > 0) {
                    const void* data = nullptr;
                    size_t n = 0;
                    if (sd_bus_message_read_array(m, SD_BUS_TYPE_BYTE, &data, &n) > 0 &&
                        data && n >= a2dp::kSbcCapsBytes) {
                        cfg = a2dp::parseSbcCaps((const uint8_t*)data, n);
                        haveCfg = true;
                    }
                    sd_bus_message_exit_container(m);
                }
            } else {
                // Delay, Volume, State, UUID, Codec: BlueZ adds properties
                // between releases and an unknown one must not abort the read.
                sd_bus_message_skip(m, "v");
            }
            sd_bus_message_exit_container(m);
        }
        sd_bus_message_exit_container(m);
    }

    {
        std::lock_guard<std::mutex> lk(self->stateMu_);
        self->transportPath_   = transport ? transport : "";
        self->transportDevice_ = device;
        // The CONFIGURATION in this message is authoritative, not the one
        // SelectConfiguration returned: BlueZ may configure us from a cached
        // choice made in an earlier session, in which case our own callback
        // was never invoked at all.
        if (haveCfg) self->chosen_ = cfg;
        self->haveTransport_ = !self->transportPath_.empty();
        self->acquired_ = false;
    }
    self->stateCv_.notify_all();

    return sd_bus_reply_method_return(m, "");
}

int Endpoint::clearConfigurationThunk(sd_bus_message* m, void* user, void* err) {
    (void)err;
    Endpoint* self = (Endpoint*)user;
    const char* transport = nullptr;
    sd_bus_message_read_basic(m, SD_BUS_TYPE_OBJECT_PATH, &transport);
    {
        std::lock_guard<std::mutex> lk(self->stateMu_);
        // Only if it is the one we are holding: BlueZ clears per transport, and
        // a stale clear for a device that already went must not drop a live one.
        if (transport && self->transportPath_ == transport) {
            self->transportPath_.clear();
            self->transportDevice_.clear();
            self->haveTransport_ = false;
            self->acquired_ = false;
        }
    }
    self->stateCv_.notify_all();
    return sd_bus_reply_method_return(m, "");
}

// BlueZ is taking the endpoint away -- the adapter went, or bluetoothd is
// restarting. Everything derived from it is now false.
int Endpoint::releaseThunk(sd_bus_message* m, void* user, void* err) {
    (void)err;
    Endpoint* self = (Endpoint*)user;
    {
        std::lock_guard<std::mutex> lk(self->stateMu_);
        self->transportPath_.clear();
        self->transportDevice_.clear();
        self->haveTransport_ = false;
        self->acquired_ = false;
        self->lastError_ = "BlueZ released the endpoint";
    }
    self->stateCv_.notify_all();
    return sd_bus_reply_method_return(m, "");
}

namespace {

// sd-bus wants int(sd_bus_message*, void*, sd_bus_error*). The four handlers
// above take a void* for that last argument so that bluez_endpoint.h can stay
// free of <systemd/sd-bus.h> -- one header include is not worth pushing sd-bus
// into every file that owns an Endpoint. These four lines are the whole cost.
int trSelect(sd_bus_message* m, void* u, sd_bus_error* e) {
    return Endpoint::selectConfigurationThunk(m, u, e);
}
int trSet(sd_bus_message* m, void* u, sd_bus_error* e) {
    return Endpoint::setConfigurationThunk(m, u, e);
}
int trClear(sd_bus_message* m, void* u, sd_bus_error* e) {
    return Endpoint::clearConfigurationThunk(m, u, e);
}
int trRelease(sd_bus_message* m, void* u, sd_bus_error* e) {
    return Endpoint::releaseThunk(m, u, e);
}

const sd_bus_vtable kEndpointVtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("SelectConfiguration", "ay", "ay", trSelect, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SetConfiguration",  "oa{sv}", "", trSet,     SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("ClearConfiguration",     "o", "", trClear,   SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Release",                 "", "", trRelease, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END
};
}  // namespace

// --- lifecycle --------------------------------------------------------------

Endpoint::Endpoint() = default;

Endpoint::~Endpoint() { stop(); }

const std::string& Endpoint::lastError() const {
    std::lock_guard<std::mutex> lk(stateMu_);
    return lastError_;
}

void Endpoint::setError(const char* what, int r) {
    std::lock_guard<std::mutex> lk(stateMu_);
    lastError_ = std::string(what) + ": " + std::string(strerror(r < 0 ? -r : r));
}

void Endpoint::setPreferredFormat(int rate, int channels) {
    std::lock_guard<std::mutex> lk(stateMu_);
    if (rate > 0)     wantRate_ = rate;
    if (channels > 0) wantChannels_ = channels;
}

bool Endpoint::start(const std::string& adapterPath, const a2dp::SbcCaps& ourCaps) {
    if (running()) return true;
    if (adapterPath.empty()) {
        std::lock_guard<std::mutex> lk(stateMu_);
        lastError_ = "no Bluetooth adapter";
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(stateMu_);
        ourCaps_ = ourCaps;
        lastError_.clear();
    }

    // A PRIVATE connection, not sd_bus_default_system(): that one is shared
    // per thread with anything else in the process that asked for it, and this
    // one is pumped by a thread of our own.
    int r = sd_bus_open_system(&bus_);
    if (r < 0) {
        bus_ = nullptr;
        setError("cannot reach the system bus", r);
        return false;
    }

    r = sd_bus_add_object_vtable(bus_, nullptr, kObjectPath, kIfaceEndpoint,
                                 kEndpointVtable, this);
    if (r < 0) {
        setError("cannot publish the media endpoint", r);
        sd_bus_unref(bus_);
        bus_ = nullptr;
        return false;
    }

    // Hand it to the adapter. From here BlueZ may call back at any time, so the
    // pump has to be running first.
    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { loop(); });

    uint8_t caps[a2dp::kSbcCapsBytes];
    a2dp::writeSbcCaps(ourCaps, caps);

    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message* call = nullptr;
    {
        std::lock_guard<std::mutex> lk(busMu_);
        r = sd_bus_message_new_method_call(bus_, &call, kBluez, adapterPath.c_str(),
                                           kIfaceMedia, "RegisterEndpoint");
        if (r >= 0) r = sd_bus_message_append(call, "o", kObjectPath);
        if (r >= 0) r = sd_bus_message_open_container(call, SD_BUS_TYPE_ARRAY, "{sv}");
        if (r >= 0) r = sd_bus_message_append(call, "{sv}", "UUID", "s", kUuidA2dpSource);
        if (r >= 0) r = sd_bus_message_append(call, "{sv}", "Codec", "y", (uint8_t)0x00);
        // Capabilities is an ay inside a variant, which sd_bus_message_append's
        // format string cannot express -- it has to be built container by
        // container.
        if (r >= 0) r = sd_bus_message_open_container(call, SD_BUS_TYPE_DICT_ENTRY, "sv");
        if (r >= 0) r = sd_bus_message_append_basic(call, SD_BUS_TYPE_STRING, "Capabilities");
        if (r >= 0) r = sd_bus_message_open_container(call, SD_BUS_TYPE_VARIANT, "ay");
        if (r >= 0) r = sd_bus_message_append_array(call, SD_BUS_TYPE_BYTE, caps, sizeof(caps));
        if (r >= 0) r = sd_bus_message_close_container(call);   // v
        if (r >= 0) r = sd_bus_message_close_container(call);   // sv
        if (r >= 0) r = sd_bus_message_close_container(call);   // a{sv}
        if (r >= 0) r = sd_bus_call(bus_, call, 0, &err, nullptr);
    }
    if (call) sd_bus_message_unref(call);

    if (r < 0) {
        {
            std::lock_guard<std::mutex> lk(stateMu_);
            lastError_ = err.message ? err.message
                                     : std::string("RegisterEndpoint failed: ") +
                                           strerror(r < 0 ? -r : r);
        }
        sd_bus_error_free(&err);
        stop();
        return false;
    }
    sd_bus_error_free(&err);
    return true;
}

void Endpoint::stop() {
    if (running_.exchange(false, std::memory_order_acq_rel)) {
        if (thread_.joinable()) thread_.join();
    } else if (thread_.joinable()) {
        thread_.join();
    }

    if (bus_) {
        // UnregisterEndpoint is a courtesy: closing the connection tells BlueZ
        // the same thing, and if bluetoothd is what went away the call would
        // block. Attempted, not depended on.
        std::lock_guard<std::mutex> lk(busMu_);
        sd_bus_flush(bus_);
        sd_bus_close(bus_);
        sd_bus_unref(bus_);
        bus_ = nullptr;
    }
    {
        std::lock_guard<std::mutex> lk(stateMu_);
        transportPath_.clear();
        transportDevice_.clear();
        haveTransport_ = false;
        acquired_ = false;
    }
    stateCv_.notify_all();
}

// The pump. sd_bus_wait() would be one line, but it blocks INSIDE the library
// and the connection can then not be used by anybody else -- and Acquire has to
// go out on this same connection (see the header). So the waiting is done in
// poll(), outside the lock, and only the processing is done under it.
void Endpoint::loop() {
    while (running_.load(std::memory_order_acquire)) {
        int fd = -1, events = 0;
        uint64_t timeoutUs = 0;
        {
            std::lock_guard<std::mutex> lk(busMu_);
            if (!bus_) break;
            // Drain first: a message already parsed into the queue will not
            // make poll() return, and would sit there for the whole slice.
            int r;
            while ((r = sd_bus_process(bus_, nullptr)) > 0) {
                if (!running_.load(std::memory_order_acquire)) return;
            }
            if (r < 0) break;
            fd     = sd_bus_get_fd(bus_);
            events = sd_bus_get_events(bus_);
            if (sd_bus_get_timeout(bus_, &timeoutUs) < 0) timeoutUs = UINT64_MAX;
        }
        if (fd < 0 || events < 0) break;

        int waitMs = kPollSliceMs;
        if (timeoutUs != UINT64_MAX) {
            const int busMs = (int)(timeoutUs / 1000);
            if (busMs >= 0 && busMs < waitMs) waitMs = busMs;
        }

        struct pollfd p{};
        p.fd     = fd;
        p.events = (short)events;
        poll(&p, 1, waitMs);
    }
}

// --- what the caller does with it -------------------------------------------

bool Endpoint::hasTransport() const {
    std::lock_guard<std::mutex> lk(stateMu_);
    return haveTransport_;
}

std::string Endpoint::transportPath() const {
    std::lock_guard<std::mutex> lk(stateMu_);
    return transportPath_;
}

std::string Endpoint::transportDevice() const {
    std::lock_guard<std::mutex> lk(stateMu_);
    return transportDevice_;
}

a2dp::SbcCaps Endpoint::configuration() const {
    std::lock_guard<std::mutex> lk(stateMu_);
    return chosen_;
}

bool Endpoint::waitForTransport(const std::string& devicePath, int timeoutMs) {
    std::unique_lock<std::mutex> lk(stateMu_);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    auto ours = [&] {
        return haveTransport_ &&
               (devicePath.empty() || transportDevice_ == devicePath);
    };
    return stateCv_.wait_until(lk, deadline, ours);
}

bool Endpoint::connectProfile(const std::string& devicePath) {
    if (!bus_ || devicePath.empty()) return false;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    int r;
    {
        std::lock_guard<std::mutex> lk(busMu_);
        r = sd_bus_call_method(bus_, kBluez, devicePath.c_str(), kIfaceDevice,
                               "ConnectProfile", &err, nullptr, "s", kUuidA2dpSink);
    }
    if (r < 0) {
        std::lock_guard<std::mutex> lk(stateMu_);
        lastError_ = err.message ? err.message : strerror(-r);
    }
    sd_bus_error_free(&err);
    return r >= 0;
}

bool Endpoint::disconnectProfile(const std::string& devicePath) {
    if (!bus_ || devicePath.empty()) return false;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    int r;
    {
        std::lock_guard<std::mutex> lk(busMu_);
        r = sd_bus_call_method(bus_, kBluez, devicePath.c_str(), kIfaceDevice,
                               "DisconnectProfile", &err, nullptr, "s", kUuidA2dpSink);
    }
    if (r < 0) {
        std::lock_guard<std::mutex> lk(stateMu_);
        lastError_ = err.message ? err.message : strerror(-r);
    }
    sd_bus_error_free(&err);
    return r >= 0;
}

bool Endpoint::acquire(Transport& out) {
    out = Transport{};

    std::string path;
    {
        std::lock_guard<std::mutex> lk(stateMu_);
        if (!haveTransport_) {
            lastError_ = "BlueZ has not configured this endpoint";
            return false;
        }
        path = transportPath_;
    }

    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;
    int r;
    {
        std::lock_guard<std::mutex> lk(busMu_);
        // "TryAcquire" would return immediately when the transport is not
        // ready; "Acquire" is what makes BlueZ bring the stream UP, which is
        // what a source has to do -- a sink waits, a source starts.
        r = sd_bus_call_method(bus_, kBluez, path.c_str(), kIfaceTransport,
                               "Acquire", &err, &reply, "");
    }
    if (r < 0) {
        {
            std::lock_guard<std::mutex> lk(stateMu_);
            lastError_ = err.message ? err.message : strerror(-r);
        }
        sd_bus_error_free(&err);
        if (reply) sd_bus_message_unref(reply);
        return false;
    }

    int fd = -1;
    uint16_t mtuRead = 0, mtuWrite = 0;
    r = sd_bus_message_read(reply, "hqq", &fd, &mtuRead, &mtuWrite);
    if (r < 0 || fd < 0) {
        sd_bus_message_unref(reply);
        sd_bus_error_free(&err);
        setError("MediaTransport1.Acquire returned no socket", r);
        return false;
    }

    // The fd belongs to the MESSAGE, which is about to be freed. Duplicating it
    // is not optional -- keeping the raw number would leave the caller writing
    // into a descriptor the library has already closed, and on a busy process
    // that number is reused.
    out.fd = ::dup(fd);
    sd_bus_message_unref(reply);
    sd_bus_error_free(&err);

    if (out.fd < 0) {
        setError("cannot duplicate the transport socket", errno);
        return false;
    }
    out.readMtu  = mtuRead;
    out.writeMtu = mtuWrite;

    {
        std::lock_guard<std::mutex> lk(stateMu_);
        acquired_ = true;
    }
    return true;
}

void Endpoint::releaseTransport() {
    std::string path;
    {
        std::lock_guard<std::mutex> lk(stateMu_);
        if (!acquired_ || transportPath_.empty()) return;
        path = transportPath_;
        acquired_ = false;
    }
    if (!bus_) return;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    {
        std::lock_guard<std::mutex> lk(busMu_);
        sd_bus_call_method(bus_, kBluez, path.c_str(), kIfaceTransport,
                           "Release", &err, nullptr, "");
    }
    sd_bus_error_free(&err);
}

}  // namespace bluez
}  // namespace ae
