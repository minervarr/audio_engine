#ifndef AE_BACKENDS_USB_IO_H
#define AE_BACKENDS_USB_IO_H

// ae::AudioSink / ae::AudioSource adapters over UsbAudioDriver.
//
// UsbAudioDriver drives BOTH directions from one object, so it can't inherit
// both interfaces directly (their configure/start/stop signatures collide).
// These thin, non-owning adapters expose each direction through the shared
// interface — the playback path as an AudioSink, the capture path as an
// AudioSource — so the engine can treat USB uniformly with the ALSA/JACK
// backends. The driver keeps its richer native API (per-type writes, hardware
// volume, DSD, latency profile) for callers that need it.

#include "usb_audio.h"
#include "core/audio_sink.h"
#include "core/audio_source.h"

// Playback: engine -> DAC. Wraps the driver's OUT pipeline.
class UsbSink : public ae::AudioSink {
public:
    explicit UsbSink(UsbAudioDriver* driver) : d_(driver) {}

    bool configure(const ae::AudioFormat& fmt) override {
        return d_->configure(fmt.sampleRate, fmt.channels, fmt.bitDepth);
    }
    bool start() override { return d_->start(); }
    int  write(const uint8_t* data, int len) override { return d_->write(data, len); }
    void stop() override { d_->stop(); }
    ae::AudioFormat activeFormat() const override {
        return { d_->getConfiguredRate(), d_->getConfiguredChannels(),
                 d_->getConfiguredBitDepth(), d_->getConfiguredSubslotSize(), false };
    }

private:
    UsbAudioDriver* d_;   // not owned
};

// Capture: ADC -> engine. Wraps the driver's IN pipeline.
class UsbSource : public ae::AudioSource {
public:
    explicit UsbSource(UsbAudioDriver* driver) : d_(driver) {}

    bool configure(const ae::AudioFormat& fmt) override {
        return d_->configureCapture(fmt.sampleRate, fmt.channels, fmt.bitDepth);
    }
    bool start() override { return d_->startCapture(); }
    int  read(uint8_t* out, int maxLen) override { return d_->readCapture(out, maxLen); }
    void stop() override { d_->stopCapture(); }
    ae::AudioFormat activeFormat() const override {
        return { d_->getConfiguredCaptureRate(), d_->getConfiguredCaptureChannels(),
                 d_->getConfiguredCaptureBitDepth(), d_->getConfiguredCaptureSubslotSize(), false };
    }

private:
    UsbAudioDriver* d_;   // not owned
};

#endif // AE_BACKENDS_USB_IO_H
