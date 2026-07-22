# MediaCodec backend (Android) — Phase 2, not yet implemented

Native Android decoding at the lowest stable NDK layer:

- `MediaCodecDecoder` — pulls encoded data via `AMediaExtractor`, decodes with
  `AMediaCodec` to interleaved PCM, exposed as a `Decoder` (a decoded-PCM
  `ae::AudioSource`).

Replaces the `MediaExtractor` / `MediaCodec` Java path currently living inside
`AudioEngine.java`. Takes a file descriptor (the host app resolves any
`content://` Uri → fd), so no `ContentResolver` and no `.java` in the engine.

Only files in this folder include `<media/NdkMediaCodec.h>` /
`<media/NdkMediaExtractor.h>`.
