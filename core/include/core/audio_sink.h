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
};

} // namespace ae

#endif // AE_CORE_AUDIO_SINK_H
