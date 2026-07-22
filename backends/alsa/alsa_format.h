#ifndef AE_BACKENDS_ALSA_FORMAT_H
#define AE_BACKENDS_ALSA_FORMAT_H

// Small ALSA <-> engine format mappings shared by the ALSA source and sink.
// Integer PCM only (16 / 24-packed / 32-bit), matching the rest of the engine.

#include <alsa/asoundlib.h>

inline snd_pcm_format_t ae_alsa_formatForBits(int bits) {
    switch (bits) {
        case 16: return SND_PCM_FORMAT_S16_LE;
        case 24: return SND_PCM_FORMAT_S24_3LE;   // packed 3-byte, no padding
        case 32: return SND_PCM_FORMAT_S32_LE;
        default: return SND_PCM_FORMAT_UNKNOWN;
    }
}

inline int ae_alsa_wireBytesForFormat(snd_pcm_format_t f) {
    switch (f) {
        case SND_PCM_FORMAT_S16_LE:  return 2;
        case SND_PCM_FORMAT_S24_3LE: return 3;
        case SND_PCM_FORMAT_S32_LE:  return 4;
        default:                     return 0;
    }
}

inline int ae_alsa_bitsForFormat(snd_pcm_format_t f) {
    switch (f) {
        case SND_PCM_FORMAT_S16_LE:  return 16;
        case SND_PCM_FORMAT_S24_3LE: return 24;
        case SND_PCM_FORMAT_S32_LE:  return 32;
        default:                     return 0;
    }
}

#endif // AE_BACKENDS_ALSA_FORMAT_H
