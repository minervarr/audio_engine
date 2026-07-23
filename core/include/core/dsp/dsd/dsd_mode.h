#ifndef AE_CORE_DSD_MODE_H
#define AE_CORE_DSD_MODE_H

namespace ae {

// How a DSD bitstream is carried to the DAC.
//   Auto   - prefer DoP (works on any DSD-capable USB DAC); native is opt-in.
//   Native - raw DSD in a 32-bit PCM container (rate = dsdRate/32).
//   Dop    - DSD-over-PCM 1.1 in 24-bit words with 0x05/0xFA markers (rate = dsdRate/16).
//   Pcm    - decimate to 16-bit PCM (speaker fallback; not bit-perfect).
enum class DsdMode { Auto, Native, Dop, Pcm };

} // namespace ae

#endif // AE_CORE_DSD_MODE_H
