#ifndef AE_CORE_AUDIO_SOURCE_H
#define AE_CORE_AUDIO_SOURCE_H

#include <cstdint>
#include "core/audio_format.h"

namespace ae {

// Input backend: a device produces PCM and the engine pulls it out here.
// Implemented by the USB ADC path (all platforms) and the desktop ALSA/JACK
// capture backends. Lifecycle: configure -> start -> read* -> stop.
class AudioSource {
public:
    virtual ~AudioSource() = default;

    // Negotiate the capture format. The backend may adjust; query the effective
    // values via activeFormat() afterwards. Returns false if unsupported.
    virtual bool configure(const AudioFormat& fmt) = 0;

    // Begin capturing. Must follow a successful configure().
    virtual bool start() = 0;

    // Drain up to maxLen bytes of interleaved PCM in the active format into out.
    // Returns bytes produced; 0 when nothing is buffered yet; negative on a hard
    // failure (e.g. not started).
    virtual int read(uint8_t* out, int maxLen) = 0;

    // Stop capturing and release device resources.
    virtual void stop() = 0;

    // The format currently in effect (valid after configure()).
    virtual AudioFormat activeFormat() const = 0;
};

} // namespace ae

#endif // AE_CORE_AUDIO_SOURCE_H
