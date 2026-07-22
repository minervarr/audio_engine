#ifndef AE_CORE_AUDIO_FORMAT_H
#define AE_CORE_AUDIO_FORMAT_H

namespace ae {

// Describes one concrete PCM stream format on the wire. Shared vocabulary
// between the engine core and every IO backend (AudioSink / AudioSource).
struct AudioFormat {
    int  sampleRate   = 0;      // Hz
    int  channels     = 0;      // interleaved channel count
    int  bitDepth     = 0;      // significant bits per sample
    int  subslotBytes = 0;      // bytes on the wire per sample (>= (bitDepth+7)/8)
    bool isFloat      = false;  // true when samples are float32 (e.g. JACK)

    int frameBytes() const { return subslotBytes * channels; }
    bool valid() const {
        return sampleRate > 0 && channels > 0 && subslotBytes > 0;
    }
};

} // namespace ae

#endif // AE_CORE_AUDIO_FORMAT_H
