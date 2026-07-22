# AAudio backend (Android) — Phase 2, not yet implemented

Native Android audio IO at the lowest stable NDK layer:

- `AAudioSink`   — playback, implements `ae::AudioSink`
- `AAudioSource` — capture,  implements `ae::AudioSource`

Replaces the Java `AudioTrack` / `AudioRecord` paths so the engine library
ships **zero `.java`**. AAudio requires **API 26**; the minSdk 24→26 decision
(vs an OpenSL ES fallback) is settled in the Phase 2 design before code lands.

Only files in this folder include `<aaudio/AAudio.h>`.
