# CLAUDE.md

Guidance for Claude Code (claude.ai/code) working in this repository. Read this
before making architectural decisions — it captures the engine's long-term
vision so future work doesn't drift.

# audio_engine — cross-platform bit-perfect audio engine

A pure-C++ audio engine consumed as a first-party library by applications on
**Linux, Android, and Windows**. One platform-agnostic core; every platform is
an equal peer. The engine's job is the audio pipeline:

```
SOURCE  ─►  BUFFER  ─►  DSP  ─►  SINK
(device / decoder → engine)   (RT-safe)   (EQ / convert)   (engine → device)
```

The crown jewel is **direct USB** (libusb UAC1/UAC2), bypassing every OS sound
layer for a bit-perfect path; ALSA and JACK2 are the desktop device backends;
AAudio (Android) and WASAPI (Windows) follow.

## Build & test commands

```
scripts/linux/build.sh                 # desktop: cmake+ninja -> build/linux/
scripts/windows/build.ps1              # desktop Windows (parked) -> build/windows/
platform/android/gradlew assembleRelease   # Android AAR -> build via Gradle
```

Android needs: JDK 17, Android SDK (platform 35, build-tools 35.0.0),
NDK 29.0.14206865, CMake 3.22.1. No Android Studio — the Gradle project is
CLI-driven (wrapper committed). Desktop needs cmake ≥ 3.22, ninja, a C++17
compiler, ALSA headers, and jack2 dev headers (NEVER pipewire-jack).

## Architecture — the big picture

**The seam: `ae::Engine` (C++) behind a stable C ABI (`api/include/audio_engine.h`).**
Every host app on every OS talks only to this C boundary. The engine is 100%
modern C++ inside; the boundary is `extern "C"` for ABI stability and trivial
binding (JNI, P/Invoke, machine-generated glue). A host app cannot tell — and
must not need to know — what platform backends are underneath.

**IO is abstracted by three interfaces in `core/include/core/`:**
`ae::AudioSource` (device/decoder → engine), `ae::AudioSink` (engine → device),
and the decoder seam. Backends implement these; the engine orchestrates them.

## Architectural directives (agreed — do not drift)

1. **Monorepo separated by concern**, root named after the product:
   - `core/` — pure C++17, `std::` only, **zero OS/JVM/third-party headers**.
     Public contracts in `core/include/core/`, impl in `core/src/`.
   - `api/` — the shipped **C ABI** (`include/audio_engine.h`) + its C++ impl
     (`src/`), wrapping `ae::Engine`.
   - `backends/` — OS/library-specific IO. `usb/` `alsa/` `jack/` (desktop),
     `aaudio/` `mediacodec/` (Android), `wasapi/` (Windows, parked).
   - `platform/` — per-platform build systems that need **more than a
     compiler** (currently `android/`, a self-contained Gradle project).
   - `scripts/` — thin build entry points for platforms that need only
     cmake+ninja+a compiler (`linux/`, `windows/`). **No source, ever.**
   - `tools/` — desktop smoke-test executables. `tests/`, `third_party/`.
2. **Driver-abstraction / single-includer rule.** `core/` includes no OS or
   library headers. Each backend is the **only** place that includes its
   library: only `backends/alsa/*` includes `<alsa/asoundlib.h>`, only
   `backends/mediacodec/*` includes `<media/NdkMediaCodec.h>`, etc.
3. **Public vs private headers.** A module's public contracts live in
   `<module>/include/<module>/`; implementation in `<module>/src/`. Consumers
   include the self-documenting path, e.g. `#include "core/audio_sink.h"`.
4. **Platform isolation, ~95% platform-agnostic.** All platforms are EQUAL
   PEERS; each has its own build entry (`platform/android/` or
   `scripts/<os>/`) outputting to `build/<platform>/`. **Never** put
   platform-named files at the repo root; no platform is "the" project. Small
   OS quirks inside an otherwise-portable component go in an `os/` subfolder.
5. **The engine library ships 100% C++ — zero `.java`.** On Android, decode is
   `AMediaCodec`, output/capture is AAudio, USB is libusb over a file
   descriptor. Any JNI bridge is written in **C++** (JNIEnv from `<jni.h>`),
   not a `.java` file. The only Java that ever exists is in the *consuming
   app* (its UI, and resolving a `content://` Uri → fd) — never in the engine.
   This is "Java only where the OS forces the app's hand," honored completely.
6. **AI-friendly public surface.** Flat C ABI, opaque handle, integer result
   codes + `ae_result_str()`. Explicit units in names (`_hz`, `_ms`, `_db_q8`,
   `_bytes`). One responsibility per file, small files. Doc-comments state
   threading, ownership, and pre/postconditions.
7. **JACK must be real jack2 `libjack`, NEVER pipewire-jack.** Verify after
   building: `ldd build/linux/<jack tool> | grep jack` must resolve to
   jack2's `libjack.so.0` and never mention pipewire.
8. **Manifest rule.** Folder names describe *architecture*; project metadata
   lives in root `manifest.json`, never encoded in folder names.

## Phasing

- **Phase 1 ✅** — restructure (`core`/`backends`/`tools`/`platform`/`scripts`),
  unified desktop build, `AudioSource`/`AudioSink` interfaces, and full-duplex
  ALSA/JACK (added the output sinks). Verified on real hardware + jackd2.
- **Phase 2 (in progress)** — adopt the seam architecture above (this doc), then
  port Android off Java: `ae::Engine` + C ABI, native decode (`AMediaCodec`),
  native output (AAudio), native DSD, retiring `AudioEngine.java`. The
  minSdk 24→26 (AAudio) vs OpenSL ES decision is settled in the Phase 2 design.
- **Phase 3 (parked)** — Windows WASAPI backend behind the same interfaces.

## Conventions

- C++17, CMake ≥ 3.22. Dependencies vendored or submodules; no package managers.
- Design docs live in `docs/superpowers/specs/`; plans in `~/.claude/plans/`.
- Commit only when asked; never commit `build/`, `.gradle/`, `.cxx/`, or
  `local.properties` (see `platform/android/.gitignore`).
