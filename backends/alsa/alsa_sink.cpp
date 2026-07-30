#include "alsa_sink.h"
#include "alsa_format.h"

#include <alsa/asoundlib.h>

#include <cstdio>
#include <chrono>

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

    // Ask for the buffer FIRST and derive the period from what we actually got:
    // the buffer is the whole cushion here, so it is the value that matters.
    // (The old code did the reverse — a 20 ms period times ten — which capped
    // the cushion at 200 ms regardless of what the hardware could offer.)
    snd_pcm_uframes_t bufSize = (snd_pcm_uframes_t)((uint64_t)rate * kBufferMs / 1000);
    err = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &bufSize);
    if (err < 0) { LOGE("set_buffer_size_near: %s", snd_strerror(err)); return false; }
    snd_pcm_uframes_t period = bufSize / kPeriodDiv;
    err = snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, nullptr);
    if (err < 0) { LOGE("set_period_size_near: %s", snd_strerror(err)); return false; }

    err = snd_pcm_hw_params(pcm, hw);
    if (err < 0) { LOGE("hw_params commit: %s", snd_strerror(err)); return false; }

    // Read back what the hardware granted — set_*_near may clamp hard.
    snd_pcm_uframes_t gotBuf = 0, gotPeriod = 0;
    if (snd_pcm_get_params(pcm, &gotBuf, &gotPeriod) < 0) { gotBuf = bufSize; gotPeriod = period; }
    bufferFrames_ = (unsigned)gotBuf;
    periodFrames_ = (unsigned)gotPeriod;

    // Software params were never set before, so start_threshold was ALSA's
    // default of 1 frame: the DAC began consuming on the very first write, with
    // an empty buffer, and ran on a razor-thin margin from then on. Starting
    // only once the buffer is primed is what actually makes the cushion real.
    //
    // Deliberately TWO PERIODS, not the whole buffer. This threshold governs
    // startup only — in steady state the writer blocks on a full buffer, so the
    // cushion is the full kBufferMs either way. Demanding the whole buffer would
    // strand any run of audio shorter than it: seek to within 2 s of the end of
    // a track and the decoder could never reach the mark, so playback would
    // never start at all.
    snd_pcm_sw_params_t* sw = nullptr;
    snd_pcm_sw_params_alloca(&sw);
    if (snd_pcm_sw_params_current(pcm, sw) >= 0) {
        snd_pcm_sw_params_set_start_threshold(pcm, sw, gotPeriod * 2);
        snd_pcm_sw_params_set_avail_min(pcm, sw, gotPeriod);
        if ((err = snd_pcm_sw_params(pcm, sw)) < 0)
            LOGE("sw_params commit: %s", snd_strerror(err));
    }

    fmt_.sampleRate   = (int)rate;
    fmt_.channels     = (int)ch;
    fmt_.bitDepth     = ae_alsa_bitsForFormat(pcmFmt);
    fmt_.subslotBytes = ae_alsa_wireBytesForFormat(pcmFmt);
    fmt_.isFloat      = false;
    configured = true;

    LOGI("configured %d Hz, %d ch, %d-bit (%d bytes/sample on the wire)",
         fmt_.sampleRate, fmt_.channels, fmt_.bitDepth, fmt_.subslotBytes);
    LOGI("device buffer %u frames (%.0f ms), period %u frames (%.0f ms)",
         bufferFrames_, bufferFrames_ * 1000.0 / fmt_.sampleRate,
         periodFrames_, periodFrames_ * 1000.0 / fmt_.sampleRate);
    if (bufferFrames_ * 1000.0 / fmt_.sampleRate < kBufferMs * 0.5)
        LOGI("note: hardware capped the buffer well under the %d ms requested",
             kBufferMs);
    if ((int)rate != sampleRate) LOGI("note: rate %d adjusted to %d", sampleRate, fmt_.sampleRate);
    if ((int)ch != channels)     LOGI("note: channels %d adjusted to %d", channels, fmt_.channels);
    return true;
}

bool AlsaSink::start() {
    if (!configured || streaming.load()) return false;
    int err = snd_pcm_prepare(pcm);
    if (err < 0) { LOGE("snd_pcm_prepare: %s", snd_strerror(err)); return false; }
    underruns.store(0);
    lastReportedUnderruns_ = 0;
    faulted.store(false, std::memory_order_release);
    streaming.store(true, std::memory_order_release);
    return true;
}

int AlsaSink::write(const uint8_t* data, int len) {
    if (!streaming.load(std::memory_order_acquire) || !pcm) return -1;
    const int frameBytes = fmt_.frameBytes();
    if (frameBytes <= 0) return -1;

    reportUnderruns();

    const int frames = len / frameBytes;
    int done = 0;
    while (done < frames) {
        snd_pcm_sframes_t w = snd_pcm_writei(
            pcm, data + (size_t)done * frameBytes, (snd_pcm_uframes_t)(frames - done));
        if (w < 0) {
            // -EPIPE (underrun) / -ESTRPIPE (suspended): recover and retry.
            // Count it FIRST — snd_pcm_recover is called silently (the trailing
            // 1), so this counter is the only record that audio dropped out.
            if (w == -EPIPE) underruns.fetch_add(1, std::memory_order_relaxed);
            w = snd_pcm_recover(pcm, (int)w, 1);
            if (w < 0) {
                LOGE("snd_pcm_writei: %s", snd_strerror((int)w));
                faulted.store(true, std::memory_order_release);
                break;
            }
            continue;
        }
        done += (int)w;
    }
    return done * frameBytes;
}

// Called from the producing thread (write()), never from an interrupt context.
// Rate-limited to ~1/s, matching the [USB][WARN] pattern in gui/src/audio_output.h.
void AlsaSink::reportUnderruns() {
    const int n = underruns.load(std::memory_order_relaxed);
    if (n == lastReportedUnderruns_) return;
    const int64_t nowMs = (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (nowMs - lastUnderrunReportMs_ < 1000) return;
    LOGI("%d device underruns so far (+%d) — the %.0f ms buffer ran dry; "
         "audio stopped and resumed", n, n - lastReportedUnderruns_,
         fmt_.sampleRate > 0 ? bufferFrames_ * 1000.0 / fmt_.sampleRate : 0.0);
    lastReportedUnderruns_ = n;
    lastUnderrunReportMs_  = nowMs;
}

int AlsaSink::pendingFrames() const {
    if (!pcm || !streaming.load(std::memory_order_acquire)) return 0;
    snd_pcm_sframes_t delay = 0;
    if (snd_pcm_delay(pcm, &delay) < 0 || delay < 0) return 0;
    return (int)delay;
}

void AlsaSink::flush() {
    if (!pcm || !streaming.load(std::memory_order_acquire)) return;
    // drop discards the queued frames; the device then needs re-arming before
    // it will accept writes again.
    snd_pcm_drop(pcm);
    int err = snd_pcm_prepare(pcm);
    if (err < 0) LOGE("flush: snd_pcm_prepare: %s", snd_strerror(err));
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
