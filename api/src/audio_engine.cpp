#include "audio_engine.h"

#include <memory>
#include <unistd.h>

#include "core/engine.hpp"
#include "core/recorder.hpp"
#include "aaudio_sink.h"          // backends/aaudio
#include "mediacodec_decoder.h"   // backends/mediacodec
#include "flac_decoder.h"         // backends/flac
#include "flac_encoder.h"         // backends/flac
#include "mp3_decoder.h"          // backends/mp3
#include "mp3_encoder.h"          // backends/mp3
#include "usb_sink.h"             // backends/usb
#include "dsd_decoder.h"          // backends/dsd
#include "aaudio_source.h"        // backends/aaudio (capture)

namespace {
// Is this the start of an MP3 stream? Conservative on purpose so we never steal
// ADTS-AAC (which shares the 0xFFF sync): accept an ID3v2 tag, or a raw MPEG
// *Layer III* frame header (sync 0xFFEx, non-reserved version, layer bits == 01).
bool sniff_mp3(const unsigned char* m, int n) {
    if (n >= 3 && m[0] == 'I' && m[1] == 'D' && m[2] == '3') return true;   // ID3v2
    if (n >= 2 && m[0] == 0xFF && (m[1] & 0xE0) == 0xE0) {
        int version = (m[1] >> 3) & 0x03;   // 01 = reserved
        int layer   = (m[1] >> 1) & 0x03;   // 01 = Layer III
        if (version != 0x01 && layer == 0x01) return true;
    }
    return false;
}

// Pick a decoder by sniffing the stream: native FLAC ("fLaC") and native MP3 go
// to our own libFLAC / libmpg123 decoders (identical on every OEM — FLAC is
// bit-perfect, MP3 escapes the per-vendor codec lottery); everything else, and
// any native-decoder open failure, falls back to the platform AMediaCodec.
// Returns an opened decoder, or nullptr on failure.
std::unique_ptr<ae::Decoder> make_decoder(int fd, int64_t offset, int64_t length) {
    unsigned char magic[4] = {0};
    int n = (int)::pread(fd, magic, 4, offset < 0 ? 0 : offset);

    if (n == 4 && magic[0] == 'f' && magic[1] == 'L' && magic[2] == 'a' && magic[3] == 'C') {
        std::unique_ptr<ae::FlacDecoder> d(new ae::FlacDecoder());
        if (d->open(fd, offset, length)) return d;
        return nullptr;   // it really is FLAC; don't mis-route to MediaCodec
    }
    if (sniff_mp3(magic, n)) {
        std::unique_ptr<ae::Mp3Decoder> d(new ae::Mp3Decoder());
        if (d->open(fd, offset, length)) return d;
        // fall through: sniff can misfire on junk-prefixed streams -> let the OS try
    }
    std::unique_ptr<ae::MediaCodecDecoder> d(new ae::MediaCodecDecoder());
    if (d->open(fd, offset, length)) return d;
    return nullptr;
}
} // namespace

// The opaque handle: an ae::Engine plus the C callback trampolines.
struct ae_engine {
    std::unique_ptr<ae::Engine> engine;
    ae_prepared_cb   on_prepared   = nullptr;
    ae_completion_cb on_completion = nullptr;
    ae_error_cb      on_error      = nullptr;
    ae_transition_cb on_transition = nullptr;
    void*            user          = nullptr;
};

static ae_state to_c_state(ae::Engine::State s) {
    switch (s) {
        case ae::Engine::State::Idle:     return AE_STATE_IDLE;
        case ae::Engine::State::Prepared: return AE_STATE_PREPARED;
        case ae::Engine::State::Playing:  return AE_STATE_PLAYING;
        case ae::Engine::State::Paused:   return AE_STATE_PAUSED;
        case ae::Engine::State::Stopped:  return AE_STATE_STOPPED;
    }
    return AE_STATE_IDLE;
}

extern "C" {

const char* ae_result_str(ae_result code) {
    switch (code) {
        case AE_OK:               return "ok";
        case AE_ERR_UNKNOWN:      return "unknown error";
        case AE_ERR_INVALID_ARG:  return "invalid argument";
        case AE_ERR_UNSUPPORTED:  return "unsupported";
        case AE_ERR_DEVICE:       return "device error";
        case AE_ERR_DECODE:       return "decode error";
        case AE_ERR_STATE:        return "invalid state";
    }
    return "unrecognized result code";
}

uint32_t ae_abi_version(void) { return 5; }

ae_engine* ae_engine_create(void) {
    std::unique_ptr<ae::AudioSink> sink(new ae::AAudioSink());
    ae_engine* h = new (std::nothrow) ae_engine();
    if (!h) return nullptr;
    h->engine.reset(new ae::Engine(std::move(sink)));
    // Trampoline the engine's std::function callbacks out to the C callbacks.
    h->engine->setOnPrepared([h]   { if (h->on_prepared)   h->on_prepared(h->user); });
    h->engine->setOnCompletion([h] { if (h->on_completion) h->on_completion(h->user); });
    h->engine->setOnError([h](int c){ if (h->on_error)      h->on_error(c, h->user); });
    h->engine->setOnTransition([h] { if (h->on_transition) h->on_transition(h->user); });
    return h;
}

void ae_engine_destroy(ae_engine* engine) {
    if (!engine) return;
    engine->engine.reset();
    delete engine;
}

void ae_engine_set_callbacks(ae_engine* engine,
                             ae_prepared_cb on_prepared,
                             ae_completion_cb on_completion,
                             ae_error_cb on_error,
                             void* user) {
    if (!engine) return;
    engine->on_prepared   = on_prepared;
    engine->on_completion = on_completion;
    engine->on_error      = on_error;
    engine->user          = user;
}

void ae_engine_set_transition_cb(ae_engine* engine, ae_transition_cb on_transition) {
    if (!engine) return;
    engine->on_transition = on_transition;
}

ae_result ae_engine_play_fd(ae_engine* engine, int fd, int64_t offset, int64_t length) {
    if (!engine || !engine->engine || fd < 0) return AE_ERR_INVALID_ARG;
    auto dec = make_decoder(fd, offset, length);
    if (!dec) return AE_ERR_DECODE;
    return engine->engine->play(std::move(dec)) ? AE_OK : AE_ERR_DEVICE;
}

ae_result ae_engine_play_dsd_fd(ae_engine* engine, int fd, int64_t offset, int64_t length) {
    if (!engine || !engine->engine || fd < 0) return AE_ERR_INVALID_ARG;
    std::unique_ptr<ae::DsdDecoder> dec(new ae::DsdDecoder());
    if (!dec->open(fd, offset, length)) return AE_ERR_DECODE;
    return engine->engine->play(std::move(dec)) ? AE_OK : AE_ERR_DEVICE;
}

ae_result ae_engine_enqueue_next_fd(ae_engine* engine, int fd, int64_t offset, int64_t length) {
    if (!engine || !engine->engine || fd < 0) return AE_ERR_INVALID_ARG;
    auto dec = make_decoder(fd, offset, length);
    if (!dec) return AE_ERR_DECODE;
    engine->engine->enqueueNext(std::move(dec));
    return AE_OK;
}

ae_result ae_engine_seek_ms(ae_engine* engine, int64_t position_ms) {
    if (!engine || !engine->engine) return AE_ERR_INVALID_ARG;
    return engine->engine->seekMs(position_ms) ? AE_OK : AE_ERR_STATE;
}

int64_t ae_engine_duration_ms(ae_engine* engine) {
    if (!engine || !engine->engine) return -1;
    return engine->engine->durationMs();
}

ae_result ae_engine_pause(ae_engine* engine) {
    if (!engine || !engine->engine) return AE_ERR_INVALID_ARG;
    engine->engine->pause();
    return AE_OK;
}

ae_result ae_engine_resume(ae_engine* engine) {
    if (!engine || !engine->engine) return AE_ERR_INVALID_ARG;
    engine->engine->resume();
    return AE_OK;
}

ae_result ae_engine_stop(ae_engine* engine) {
    if (!engine || !engine->engine) return AE_ERR_INVALID_ARG;
    engine->engine->stop();
    return AE_OK;
}

ae_state ae_engine_state(ae_engine* engine) {
    if (!engine || !engine->engine) return AE_STATE_IDLE;
    return to_c_state(engine->engine->state());
}

int64_t ae_engine_position_ms(ae_engine* engine) {
    if (!engine || !engine->engine) return 0;
    return engine->engine->positionMs();
}

ae_result ae_engine_switch_to_usb_fd(ae_engine* engine, int fd) {
    if (!engine || !engine->engine || fd < 0) return AE_ERR_INVALID_ARG;
    std::unique_ptr<ae::UsbAudioSink> sink(new ae::UsbAudioSink());
    if (!sink->openFd(fd)) return AE_ERR_DEVICE;
    return engine->engine->switchSink(std::move(sink)) ? AE_OK : AE_ERR_DEVICE;
}

ae_result ae_engine_switch_to_speaker(ae_engine* engine) {
    if (!engine || !engine->engine) return AE_ERR_INVALID_ARG;
    std::unique_ptr<ae::AudioSink> sink(new ae::AAudioSink());
    return engine->engine->switchSink(std::move(sink)) ? AE_OK : AE_ERR_DEVICE;
}

ae_result ae_engine_set_volume(ae_engine* engine, float linear01) {
    if (!engine || !engine->engine) return AE_ERR_INVALID_ARG;
    engine->engine->setVolume(linear01);
    return AE_OK;
}

ae_result ae_engine_set_volume_mode(ae_engine* engine, ae_volume_mode mode) {
    if (!engine || !engine->engine) return AE_ERR_INVALID_ARG;
    engine->engine->setVolumeMode((int)mode);
    return AE_OK;
}

int ae_engine_has_hardware_volume(ae_engine* engine) {
    if (!engine || !engine->engine) return 0;
    return engine->engine->hasHardwareVolume() ? 1 : 0;
}

ae_result ae_engine_set_eq(ae_engine* engine, int num_filters,
                           const double* coeffs, double preamp, int enabled) {
    if (!engine || !engine->engine) return AE_ERR_INVALID_ARG;
    if (num_filters > 0 && !coeffs)  return AE_ERR_INVALID_ARG;
    engine->engine->setEq(num_filters, coeffs, preamp, enabled != 0);
    return AE_OK;
}

// --- capture -----------------------------------------------------------------

struct ae_capture {
    std::unique_ptr<ae::AudioSource> source;
};

ae_capture* ae_capture_create(void) {
    ae_capture* h = new (std::nothrow) ae_capture();
    if (!h) return nullptr;
    h->source.reset(new ae::AAudioSource());
    return h;
}

void ae_capture_destroy(ae_capture* capture) {
    if (!capture) return;
    if (capture->source) capture->source->stop();
    delete capture;
}

ae_result ae_capture_configure(ae_capture* capture, int sample_rate_hz, int channels) {
    if (!capture || !capture->source) return AE_ERR_INVALID_ARG;
    ae::AudioFormat fmt{};
    fmt.sampleRate   = sample_rate_hz;
    fmt.channels     = channels;
    fmt.bitDepth     = 16;
    fmt.subslotBytes = 2;
    return capture->source->configure(fmt) ? AE_OK : AE_ERR_DEVICE;
}

ae_result ae_capture_start(ae_capture* capture) {
    if (!capture || !capture->source) return AE_ERR_INVALID_ARG;
    return capture->source->start() ? AE_OK : AE_ERR_DEVICE;
}

int ae_capture_read(ae_capture* capture, uint8_t* out, int max_bytes) {
    if (!capture || !capture->source || !out || max_bytes <= 0) return -1;
    return capture->source->read(out, max_bytes);
}

ae_result ae_capture_stop(ae_capture* capture) {
    if (!capture || !capture->source) return AE_ERR_INVALID_ARG;
    capture->source->stop();
    return AE_OK;
}

// --- recording (mic -> FLAC / MP3 file) --------------------------------------

struct ae_recorder {
    std::unique_ptr<ae::Recorder> recorder;
};

ae_recorder* ae_recorder_create(ae_rec_codec codec) {
    ae_recorder* h = new (std::nothrow) ae_recorder();
    if (!h) return nullptr;
    std::unique_ptr<ae::AudioSource> src(new ae::AAudioSource());
    std::unique_ptr<ae::Encoder>     enc;
    switch (codec) {
        case AE_REC_MP3:  enc.reset(new ae::Mp3Encoder());  break;
        case AE_REC_FLAC:
        default:          enc.reset(new ae::FlacEncoder()); break;
    }
    h->recorder.reset(new ae::Recorder(std::move(src), std::move(enc)));
    return h;
}

void ae_recorder_destroy(ae_recorder* recorder) {
    if (!recorder) return;
    if (recorder->recorder) recorder->recorder->stop();
    delete recorder;
}

ae_result ae_recorder_start(ae_recorder* recorder, int fd, int sample_rate_hz, int channels) {
    if (!recorder || !recorder->recorder || fd < 0) return AE_ERR_INVALID_ARG;
    ae::AudioFormat fmt{};
    fmt.sampleRate   = sample_rate_hz;
    fmt.channels     = channels;
    fmt.bitDepth     = 16;
    fmt.subslotBytes = 2;
    return recorder->recorder->start(fd, fmt) ? AE_OK : AE_ERR_DEVICE;
}

ae_result ae_recorder_stop(ae_recorder* recorder) {
    if (!recorder || !recorder->recorder) return AE_ERR_INVALID_ARG;
    recorder->recorder->stop();
    return AE_OK;
}

} // extern "C"
