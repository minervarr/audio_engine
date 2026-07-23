#ifndef AE_BACKENDS_USB_SINK_H
#define AE_BACKENDS_USB_SINK_H

// UsbAudioSink — the engine's output path to a direct-USB DAC (all platforms).
//
// Unlike the thin, non-owning UsbSink adapter in usb_io.h (which desktop tools
// use over a driver they already hold), this sink OWNS a UsbAudioDriver, opens
// it from a device file descriptor, and carries the volume *policy* that the
// Android Java UsbAudioOutput used to own: VolumeMode resolution, the cube-root
// dB taper, and the hardware-FU / software-gain split. It also routes writes
// through the driver's encoding-specific writer (so software gain applies) and
// upmixes mono -> the DAC's wire channel count when the DAC lacks a mono alt.
//
// fd ownership: the caller (the host app's USB permission/connection layer)
// retains the fd and must keep it valid for the life of this sink — mirroring
// the Java path where the app's UsbDeviceConnection stays open while streaming.

#include <cstdint>
#include <memory>
#include <vector>

#include "core/audio_sink.h"

class UsbAudioDriver;

namespace ae {

class UsbAudioSink : public AudioSink {
public:
    UsbAudioSink();
    ~UsbAudioSink() override;

    // Open the USB device on `fd` (does not take ownership; see header note).
    // Must succeed before configure(). Returns false on open/descriptor error.
    bool openFd(int fd);

    bool configure(const AudioFormat& fmt) override;
    bool start() override;
    int  write(const uint8_t* data, int len) override;
    void stop() override;
    AudioFormat activeFormat() const override;

    void pause() override;
    void resume() override;
    void flush() override;
    int  pendingPlaybackMs() const override;

    void setVolume(float linear01) override;
    void setVolumeMode(int mode) override;
    bool hasHardwareVolume() const override;

private:
    int  effectiveMode() const;   // resolves VOL_AUTO to HARDWARE/SOFTWARE
    void applyCurrentVolume();

    std::unique_ptr<UsbAudioDriver> d_;
    AudioFormat          srcFmt_{};        // format the engine feeds us (decoder PCM)
    int                  volumeMode_   = VOL_AUTO;
    float                volumeLinear_ = 1.0f;   // unity = bit-perfect
    std::vector<uint8_t> expand_;          // mono->wire upmix scratch (write thread)
};

} // namespace ae

#endif // AE_BACKENDS_USB_SINK_H
