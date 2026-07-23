#ifndef AE_CORE_AUDIO_SINK_H
#define AE_CORE_AUDIO_SINK_H

#include <cstdint>
#include "core/audio_format.h"

namespace ae {

// Output backend: the engine pushes PCM here and it reaches a device.
// Implemented by the USB DAC driver (all platforms) and the desktop ALSA/JACK
// output backends; in Phase 2 the Android speaker path (AAudio) implements it
// too. Lifecycle: configure -> start -> write* -> stop.
class AudioSink {
public:
    virtual ~AudioSink() = default;

    // Negotiate the stream format. The backend may adjust; query the effective
    // values via activeFormat() afterwards. Returns false if unsupported.
    virtual bool configure(const AudioFormat& fmt) = 0;

    // Begin streaming. Must follow a successful configure().
    virtual bool start() = 0;

    // Enqueue interleaved PCM in the active format. Returns the number of bytes
    // actually consumed (may be < len when the backend's buffer is full).
    virtual int write(const uint8_t* data, int len) = 0;

    // Stop streaming and release device resources. configure() may follow to
    // restart with a new format.
    virtual void stop() = 0;

    // The format currently in effect (valid after configure()).
    virtual AudioFormat activeFormat() const = 0;

    // --- transport (optional) -------------------------------------------------
    // Playback backends that support hardware pause override these; the default
    // no-ops suit backends (files/tools) that don't. Additive: existing sinks
    // need not change.
    virtual void pause() {}
    virtual void resume() {}

    // Discard any audio already handed over but not yet rendered.
    virtual void flush() {}

    // Milliseconds of audio handed to the device but not yet rendered (driver /
    // ring / queue latency). The engine waits for this to reach ~0 at
    // end-of-track so a deeply-buffered device (e.g. a USB DAC) doesn't get its
    // queued tail discarded. Shallow backends return 0.
    virtual int pendingPlaybackMs() const { return 0; }

    // --- volume (optional) ----------------------------------------------------
    // Volume is a *sink* concern. The USB DAC sink applies it via the UAC
    // Feature Unit (hardware) and/or a software gain in its write path; the
    // Android speaker path (AAudio) defers to the OS stream volume and ignores
    // these. `linear01` is a 0..1 slider position the sink maps to a perceptual
    // (cube-root) dB taper — 1.0 is unity / bit-perfect. `mode` picks the path.
    enum VolumeMode { VOL_AUTO = 0, VOL_HARDWARE = 1, VOL_SOFTWARE = 2, VOL_EXTERNAL = 3 };
    virtual void setVolume(float linear01) { (void)linear01; }
    virtual void setVolumeMode(int mode)   { (void)mode; }
    virtual bool hasHardwareVolume() const { return false; }
};

} // namespace ae

#endif // AE_CORE_AUDIO_SINK_H
