#include "usb_sink.h"

#include <algorithm>
#include <cmath>

#include "usb_audio.h"

namespace ae {

namespace {
// Perceptual floor for the software taper and the hardware-range clamp, matching
// the Java UsbAudioOutput: slider 0 => hard mute, slider 1 => unity (bit-perfect).
constexpr double kSoftwareDbFloor = -60.0;

// Expand each PCM frame from srcCh to dstCh channels (dstCh >= srcCh) by copying
// channel 0 into every extra output channel. Mirrors UsbAudioOutput.expandChannels.
void expandChannels(const uint8_t* in, int inLen, uint8_t* out,
                    int bytesPerSample, int srcCh, int dstCh) {
    const int srcFrameBytes = srcCh * bytesPerSample;
    int o = 0;
    for (int i = 0; i + srcFrameBytes <= inLen; i += srcFrameBytes) {
        for (int b = 0; b < srcFrameBytes; b++) out[o++] = in[i + b];
        for (int e = 0; e < dstCh - srcCh; e++)
            for (int b = 0; b < bytesPerSample; b++) out[o++] = in[i + b];
    }
}
} // namespace

UsbAudioSink::UsbAudioSink() : d_(new UsbAudioDriver()) {}

UsbAudioSink::~UsbAudioSink() {
    if (d_) { d_->stop(); d_->close(); }
}

bool UsbAudioSink::openFd(int fd) {
    if (!d_) return false;
    if (!d_->open(fd)) return false;
    if (!d_->parseDescriptors()) return false;
    return true;
}

bool UsbAudioSink::configure(const AudioFormat& fmt) {
    if (!d_) return false;
    srcFmt_ = fmt;
    // Request the source's native bit depth; the driver upscales into whatever
    // subslot the DAC negotiates (source stays 16-bit PCM from the decoder).
    int requestedBitDepth = fmt.bitDepth > 0 ? fmt.bitDepth : 16;
    if (!d_->configure(fmt.sampleRate, fmt.channels, requestedBitDepth)) return false;
    // Re-assert volume on the freshly (re)configured device.
    applyCurrentVolume();
    return true;
}

bool UsbAudioSink::start() { return d_ && d_->start(); }

int UsbAudioSink::write(const uint8_t* data, int len) {
    if (!d_) return -1;
    // DSD (DoP/native) is already packed to the wire subslot by the decoder and
    // must not be gain-scaled or requantized: raw passthrough. write() returns
    // bytes consumed, which is exactly what the engine's byte loop expects.
    if (srcFmt_.isDsd) return d_->write(data, len);

    // Route by the decoder's native depth so the DAC stays bit-perfect. Each
    // driver writer applies software gain + subslot upscaling and returns the
    // number of channel-samples consumed; the engine wants bytes, so multiply by
    // the source subslot. 16-bit FLAC/PCM is bit-perfect; 24-bit FLAC reaches a
    // 24-bit DAC untouched via writeInt24Packed.
    const int subslot = srcFmt_.subslotBytes > 0 ? srcFmt_.subslotBytes : 2;
    const int srcCh   = srcFmt_.channels;
    const int wireCh  = d_->getConfiguredChannels();

    // Mono->wire upmix (16-bit path only; hi-res content is effectively stereo).
    if (subslot == 2 && wireCh > srcCh && srcCh > 0) {
        const int srcFrameBytes  = srcCh  * 2;
        const int frames         = len / srcFrameBytes;
        if (frames <= 0) return 0;
        const int wireFrameBytes = wireCh * 2;
        const size_t outBytes    = (size_t)frames * wireFrameBytes;
        if (expand_.size() < outBytes) expand_.resize(outBytes);
        expandChannels(data, frames * srcFrameBytes, expand_.data(), 2, srcCh, wireCh);
        int wireSamples = d_->writeInt16(
            reinterpret_cast<const int16_t*>(expand_.data()), (int)(outBytes / 2));
        if (wireSamples <= 0) return wireSamples;
        // Report consumption in the source-channel domain so the engine's byte
        // bookkeeping stays correct across partial writes.
        int framesWritten = wireSamples / wireCh;
        return framesWritten * srcFrameBytes;
    }

    int consumed;   // channel-samples consumed
    switch (subslot) {
        case 3:  consumed = d_->writeInt24Packed(data, len); break;                 // numBytes
        case 4:  consumed = d_->writeInt32(reinterpret_cast<const int32_t*>(data), len / 4); break;
        default: consumed = d_->writeInt16(reinterpret_cast<const int16_t*>(data), len / 2); break;
    }
    if (consumed < 0) return consumed;
    return consumed * subslot;
}

void UsbAudioSink::stop() { if (d_) d_->stop(); }

AudioFormat UsbAudioSink::activeFormat() const {
    if (!d_) return {};
    return { d_->getConfiguredRate(), d_->getConfiguredChannels(),
             d_->getConfiguredBitDepth(), d_->getConfiguredSubslotSize(), false };
}

void UsbAudioSink::pause()  { if (d_) d_->setPaused(true); }
void UsbAudioSink::resume() { if (d_) d_->setPaused(false); }
void UsbAudioSink::flush()  { if (d_) d_->flush(); }

int UsbAudioSink::pendingPlaybackMs() const {
    return d_ ? d_->getPendingPlaybackMs() : 0;
}

// --- volume ------------------------------------------------------------------

bool UsbAudioSink::hasHardwareVolume() const {
    return d_ && d_->hasHardwareVolume();
}

int UsbAudioSink::effectiveMode() const {
    if (volumeMode_ != VOL_AUTO) return volumeMode_;
    return hasHardwareVolume() ? VOL_HARDWARE : VOL_SOFTWARE;
}

void UsbAudioSink::setVolume(float linear01) {
    if (std::isnan(linear01)) linear01 = 0.0f;
    volumeLinear_ = std::min(1.0f, std::max(0.0f, linear01));
    applyCurrentVolume();
}

void UsbAudioSink::setVolumeMode(int mode) {
    volumeMode_ = mode;
    applyCurrentVolume();
}

void UsbAudioSink::applyCurrentVolume() {
    if (!d_) return;
    const int eff = effectiveMode();

    if (eff == VOL_EXTERNAL) {
        // DAC held at unity; a downstream amp / physical knob does attenuation.
        if (d_->hasHardwareVolume()) {
            d_->setHwVolumeDbQ8(d_->getVolumeMaxDbQ8());
            if (d_->hasHardwareMute()) d_->setHwMute(false);
        }
        d_->setSoftwareGain(1.0f);
        return;
    }

    if (eff == VOL_HARDWARE && d_->hasHardwareVolume()) {
        d_->setSoftwareGain(1.0f);
        if (volumeLinear_ <= 0.0f) {
            if (d_->hasHardwareMute()) d_->setHwMute(true);
            else d_->setHwVolumeDbQ8(d_->getVolumeMinDbQ8());
        } else {
            if (d_->hasHardwareMute()) d_->setHwMute(false);
            double dbMin = std::max(d_->getVolumeMinDbQ8() / 256.0, kSoftwareDbFloor);
            double dbMax = d_->getVolumeMaxDbQ8() / 256.0;
            double db = dbMin + (dbMax - dbMin) * std::cbrt((double)volumeLinear_);
            d_->setHwVolumeDbQ8((int)std::lround(db * 256.0));
        }
        return;
    }

    // SOFTWARE (explicit, or AUTO with no hardware Feature Unit).
    float gain;
    if (volumeLinear_ <= 0.0f) {
        gain = 0.0f;
    } else {
        double db = kSoftwareDbFloor * (1.0 - std::cbrt((double)volumeLinear_));
        gain = (float)std::pow(10.0, db / 20.0);
    }
    d_->setSoftwareGain(gain);
}

} // namespace ae
