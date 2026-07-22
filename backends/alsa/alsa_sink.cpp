#include "alsa_sink.h"
#include "alsa_format.h"

#include <alsa/asoundlib.h>

#include <cstdio>

#define LOGI(...) do { fprintf(stderr, "[AlsaSink INFO] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define LOGE(...) do { fprintf(stderr, "[AlsaSink ERR] "  __VA_ARGS__); fputc('\n', stderr); } while (0)

AlsaSink::AlsaSink() = default;

AlsaSink::~AlsaSink() {
    close();
}

bool AlsaSink::open(const std::string& deviceId) {
    if (opened) close();
    int err = snd_pcm_open(&pcm, deviceId.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        LOGE("snd_pcm_open(%s) failed: %s", deviceId.c_str(), snd_strerror(err));
        pcm = nullptr;
        return false;
    }
    opened = true;
    return true;
}

bool AlsaSink::configure(const ae::AudioFormat& req) {
    if (!opened || streaming.load()) return false;
    const int sampleRate = req.sampleRate;
    const int channels   = req.channels;
    const int bitDepth   = req.bitDepth;

    snd_pcm_hw_params_t* hw = nullptr;
    snd_pcm_hw_params_alloca(&hw);

    int err = snd_pcm_hw_params_any(pcm, hw);
    if (err < 0) { LOGE("hw_params_any: %s", snd_strerror(err)); return false; }

    err = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err < 0) { LOGE("set_access: %s", snd_strerror(err)); return false; }

    // Requested format first, then fall back through the others.
    snd_pcm_format_t pcmFmt = ae_alsa_formatForBits(bitDepth);
    if (pcmFmt == SND_PCM_FORMAT_UNKNOWN) { LOGE("unsupported bitDepth %d", bitDepth); return false; }
    if (snd_pcm_hw_params_set_format(pcm, hw, pcmFmt) < 0) {
        const snd_pcm_format_t fallbacks[] = {
            SND_PCM_FORMAT_S32_LE, SND_PCM_FORMAT_S24_3LE, SND_PCM_FORMAT_S16_LE };
        bool ok = false;
        for (snd_pcm_format_t f : fallbacks) {
            if (f != pcmFmt && snd_pcm_hw_params_set_format(pcm, hw, f) >= 0) {
                LOGI("format %d-bit refused, using %d-bit", bitDepth, ae_alsa_bitsForFormat(f));
                pcmFmt = f;
                ok = true;
                break;
            }
        }
        if (!ok) { LOGE("no supported integer PCM format"); return false; }
    }

    unsigned ch = (unsigned)channels;
    err = snd_pcm_hw_params_set_channels_near(pcm, hw, &ch);
    if (err < 0) { LOGE("set_channels_near(%d): %s", channels, snd_strerror(err)); return false; }

    unsigned rate = (unsigned)sampleRate;
    err = snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, nullptr);
    if (err < 0) { LOGE("set_rate_near(%d): %s", sampleRate, snd_strerror(err)); return false; }

    // ~20 ms periods, ~200 ms buffer: enough slack to survive scheduling jitter
    // on the producing thread without an underrun, still low enough latency.
    snd_pcm_uframes_t period = rate / 50;
    err = snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, nullptr);
    if (err < 0) { LOGE("set_period_size_near: %s", snd_strerror(err)); return false; }
    snd_pcm_uframes_t bufSize = period * 10;
    err = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &bufSize);
    if (err < 0) { LOGE("set_buffer_size_near: %s", snd_strerror(err)); return false; }

    err = snd_pcm_hw_params(pcm, hw);
    if (err < 0) { LOGE("hw_params commit: %s", snd_strerror(err)); return false; }

    fmt_.sampleRate   = (int)rate;
    fmt_.channels     = (int)ch;
    fmt_.bitDepth     = ae_alsa_bitsForFormat(pcmFmt);
    fmt_.subslotBytes = ae_alsa_wireBytesForFormat(pcmFmt);
    fmt_.isFloat      = false;
    configured = true;

    LOGI("configured %d Hz, %d ch, %d-bit (%d bytes/sample on the wire)",
         fmt_.sampleRate, fmt_.channels, fmt_.bitDepth, fmt_.subslotBytes);
    if ((int)rate != sampleRate) LOGI("note: rate %d adjusted to %d", sampleRate, fmt_.sampleRate);
    if ((int)ch != channels)     LOGI("note: channels %d adjusted to %d", channels, fmt_.channels);
    return true;
}

bool AlsaSink::start() {
    if (!configured || streaming.load()) return false;
    int err = snd_pcm_prepare(pcm);
    if (err < 0) { LOGE("snd_pcm_prepare: %s", snd_strerror(err)); return false; }
    streaming.store(true, std::memory_order_release);
    return true;
}

int AlsaSink::write(const uint8_t* data, int len) {
    if (!streaming.load(std::memory_order_acquire) || !pcm) return -1;
    const int frameBytes = fmt_.frameBytes();
    if (frameBytes <= 0) return -1;

    const int frames = len / frameBytes;
    int done = 0;
    while (done < frames) {
        snd_pcm_sframes_t w = snd_pcm_writei(
            pcm, data + (size_t)done * frameBytes, (snd_pcm_uframes_t)(frames - done));
        if (w < 0) {
            // -EPIPE (underrun) / -ESTRPIPE (suspended): recover and retry.
            w = snd_pcm_recover(pcm, (int)w, 1);
            if (w < 0) { LOGE("snd_pcm_writei: %s", snd_strerror((int)w)); break; }
            continue;
        }
        done += (int)w;
    }
    return done * frameBytes;
}

void AlsaSink::stop() {
    if (!streaming.load()) return;
    streaming.store(false, std::memory_order_release);
    if (pcm) snd_pcm_drop(pcm);
}

void AlsaSink::close() {
    stop();
    if (pcm) {
        snd_pcm_close(pcm);
        pcm = nullptr;
    }
    opened = false;
    configured = false;
}
