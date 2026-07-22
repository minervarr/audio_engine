#include "alsa_source.h"
#include "core/buffer/pcm_buffer.h"

#include <alsa/asoundlib.h>

#include <cstdio>

#include "alsa_format.h"

#define LOGI(...) do { fprintf(stderr, "[AlsaSource INFO] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define LOGE(...) do { fprintf(stderr, "[AlsaSource ERR] "  __VA_ARGS__); fputc('\n', stderr); } while (0)

AlsaSource::AlsaSource() = default;

AlsaSource::~AlsaSource() {
    close();
}

std::vector<AlsaCaptureDeviceInfo> AlsaSource::enumerateCaptureDevices() {
    std::vector<AlsaCaptureDeviceInfo> out;

    int card = -1;
    while (snd_card_next(&card) >= 0 && card >= 0) {
        char ctlName[32];
        snprintf(ctlName, sizeof(ctlName), "hw:%d", card);

        snd_ctl_t* ctl = nullptr;
        if (snd_ctl_open(&ctl, ctlName, 0) < 0) continue;

        snd_ctl_card_info_t* cardInfo = nullptr;
        snd_ctl_card_info_alloca(&cardInfo);
        const char* cardName = "(unknown card)";
        if (snd_ctl_card_info(ctl, cardInfo) >= 0) {
            cardName = snd_ctl_card_info_get_name(cardInfo);
        }

        int device = -1;
        while (snd_ctl_pcm_next_device(ctl, &device) >= 0 && device >= 0) {
            snd_pcm_info_t* pcmInfo = nullptr;
            snd_pcm_info_alloca(&pcmInfo);
            snd_pcm_info_set_device(pcmInfo, (unsigned)device);
            snd_pcm_info_set_subdevice(pcmInfo, 0);
            snd_pcm_info_set_stream(pcmInfo, SND_PCM_STREAM_CAPTURE);
            if (snd_ctl_pcm_info(ctl, pcmInfo) < 0) continue;   // not capture-capable

            AlsaCaptureDeviceInfo d;
            char id[32];
            snprintf(id, sizeof(id), "hw:%d,%d", card, device);
            d.deviceId = id;
            d.name = std::string(cardName) + " — " + snd_pcm_info_get_name(pcmInfo);
            out.push_back(std::move(d));
        }
        snd_ctl_close(ctl);
    }
    return out;
}

bool AlsaSource::open(const std::string& deviceId) {
    if (opened) close();
    int err = snd_pcm_open(&pcm, deviceId.c_str(), SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        LOGE("snd_pcm_open(%s) failed: %s", deviceId.c_str(), snd_strerror(err));
        pcm = nullptr;
        return false;
    }
    opened = true;
    return true;
}

bool AlsaSource::configure(const ae::AudioFormat& fmt) {
    if (!opened || streaming.load()) return false;
    const int sampleRate = fmt.sampleRate;
    const int channels   = fmt.channels;
    const int bitDepth   = fmt.bitDepth;

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

    // ~20 ms periods, ~200 ms hardware buffer: comfortable for a polling
    // consumer, still far below the ring's own capacity.
    snd_pcm_uframes_t period = rate / 50;
    err = snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, nullptr);
    if (err < 0) { LOGE("set_period_size_near: %s", snd_strerror(err)); return false; }
    snd_pcm_uframes_t bufSize = period * 10;
    err = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &bufSize);
    if (err < 0) { LOGE("set_buffer_size_near: %s", snd_strerror(err)); return false; }

    err = snd_pcm_hw_params(pcm, hw);
    if (err < 0) { LOGE("hw_params commit: %s", snd_strerror(err)); return false; }

    capRate = (int)rate;
    capChannels = (int)ch;
    capBitDepth = ae_alsa_bitsForFormat(pcmFmt);
    capSubslotSize = ae_alsa_wireBytesForFormat(pcmFmt);
    configured = true;

    LOGI("configured %d Hz, %d ch, %d-bit (%d bytes/sample on the wire)",
         capRate, capChannels, capBitDepth, capSubslotSize);

    if ((int)rate != sampleRate)   LOGI("note: rate %d adjusted to %d", sampleRate, capRate);
    if ((int)ch != channels)       LOGI("note: channels %d adjusted to %d", channels, capChannels);

    return true;
}

bool AlsaSource::start() {
    if (!configured || streaming.load()) return false;

    int err = snd_pcm_prepare(pcm);
    if (err < 0) { LOGE("snd_pcm_prepare: %s", snd_strerror(err)); return false; }

    // 1 s ring so a slow consumer doesn't drop samples mid-take (same
    // sizing philosophy as UsbAudioDriver's STABLE capture profile).
    size_t bytesPerSecond = (size_t)capRate * capChannels * capSubslotSize;
    ring = new NativePcmBuffer(bytesPerSecond);

    streaming.store(true, std::memory_order_release);
    readerThread = std::thread(&AlsaSource::readerThreadFn, this);
    return true;
}

void AlsaSource::readerThreadFn() {
    const int frameBytes = capChannels * capSubslotSize;
    const snd_pcm_uframes_t chunkFrames = (snd_pcm_uframes_t)(capRate / 50); // ~20 ms
    std::vector<uint8_t> buf((size_t)chunkFrames * frameBytes);

    while (streaming.load(std::memory_order_acquire)) {
        snd_pcm_sframes_t got = snd_pcm_readi(pcm, buf.data(), chunkFrames);
        if (got == -EPIPE) {
            // Overrun (consumer stalled long enough for the hw buffer to
            // fill). Recover and keep going — samples in the gap are lost.
            LOGI("overrun, recovering");
            snd_pcm_prepare(pcm);
            continue;
        }
        if (got == -EAGAIN) continue;
        if (got < 0) {
            LOGE("snd_pcm_readi: %s", snd_strerror((int)got));
            break;
        }
        if (got == 0) continue;
        // Blocks when the ring is full; flush() in stop() unblocks it.
        ring->write(buf.data(), 0, (int)(got * frameBytes));
    }
    streaming.store(false, std::memory_order_release);
}

int AlsaSource::read(uint8_t* out, int maxBytes) {
    if (!ring) return -1;
    if (!streaming.load(std::memory_order_acquire)) return -1;
    int n = ring->read(out, 0, maxBytes);
    return n < 0 ? 0 : n;   // flush/end sentinels are not hard failures here
}

void AlsaSource::stop() {
    if (!streaming.load() && !readerThread.joinable()) return;
    streaming.store(false, std::memory_order_release);
    if (pcm) snd_pcm_drop(pcm);
    if (ring) ring->flush();          // unblock a writer stuck on a full ring
    if (readerThread.joinable()) readerThread.join();
    delete ring;
    ring = nullptr;
}

void AlsaSource::close() {
    stop();
    if (pcm) {
        snd_pcm_close(pcm);
        pcm = nullptr;
    }
    opened = false;
    configured = false;
}
