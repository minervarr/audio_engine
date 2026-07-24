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
git submodule update --init --recursive    # libusb + libFLAC (submodules)
python3 initialize_files.py                 # fetch mpg123 + LAME (MP3, not committed)
scripts/linux/build.sh                 # desktop: cmake+ninja -> build/linux/
scripts/windows/build.ps1              # desktop Windows (parked) -> build/windows/
platform/android/gradlew assembleRelease   # Android AAR -> build via Gradle
```

**First-time setup:** run `git submodule update --init` (libusb, libFLAC) and
`python3 initialize_files.py` (downloads mpg123 + LAME — their source is *not*
committed; see `third_party/README.md`). The Android build hard-errors with the
fetch instruction if the MP3 source is absent.

Android needs: JDK 17, Android SDK (platform 35, build-tools 35.0.0),
NDK 29.0.14206865, CMake 3.22.1. No Android Studio — the Gradle project is
CLI-driven (wrapper committed). Desktop needs cmake ≥ 3.22, ninja, a C++17
compiler, ALSA headers, and jack2 dev headers (NEVER pipewire-jack).
`initialize_files.py` is pure Python 3 stdlib (no pip deps).

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
     `aaudio/` `mediacodec/` (Android), `wasapi/` (Windows, parked), `dsd/`
     (DFF/DSF + DoP), `flac/` (native FLAC decode + encode via vendored libFLAC),
     `mp3/` (native MP3 decode via libmpg123 + encode via LAME).
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
- **Phase 2 ✅** — Android ported off Java. `ae::Engine` (`core/`) behind the C
  ABI (`api/`); native decode (`AMediaCodec`), output/capture (AAudio), USB DAC
  sink with volume policy, gapless + seek, and native DSD (DFF/DSF parsers + DoP
  packer). minSdk 26 (AAudio-only). The engine now ships **zero `.java`**; the
  five legacy `*_jni.cpp` bridges and all `src/main/java/**` are deleted. Host
  apps talk only to `audio_engine.h` (ABI v3). Compile-verified across all four
  Android ABIs + desktop; on-device audio testing pending real hardware.
- **Phase 2.5 ✅** — Native FLAC via vendored libFLAC (BSD-3-Clause, submodule
  `third_party/flac`). `FlacDecoder` replaces `AMediaCodec` for FLAC so playback
  is identical bit-perfect on every OEM (magic-sniffed in `api/`; no ABI change);
  24-bit stays native to the USB DAC, 16-bit AAudio speaker path dithers 24→16.
  `FlacEncoder` + `ae::Recorder` add mic→PCM→`.flac` capture (`ae_recorder_*`,
  ABI v4). Android now ships **64-bit ABIs only** (`arm64-v8a`, `x86_64`) — Google
  Play's floor since 2019, and it sidesteps libFLAC's 32-bit fseeko quirk.
  libFLAC is kept pristine (configured only via CMake cache vars, never patched).
- **Phase 2.6 ✅** — Native MP3 via vendored **libmpg123** (LGPL-2.1, decode) +
  **LAME/libmp3lame** (LGPL, encode). Same motive as FLAC: MP3 escapes the
  per-OEM `AMediaCodec` lottery and decodes identically everywhere. `Mp3Decoder`
  forces signed-16 out (MP3 is lossy 16-bit, so no USB-24/dither path);
  magic-sniffed in `make_decoder` (ID3 or a Layer-III frame header, conservative
  to never steal ADTS-AAC, with a MediaCodec fallback). `Mp3Encoder` (LAME VBR
  ~V2) adds MP3 to the recorder via `ae_recorder_create(ae_rec_codec)` — ABI v5.
  Neither mpg123 nor LAME has an official Git repo, and their source is **not
  committed** — `initialize_files.py` (pure-stdlib, cross-platform) downloads the
  pinned official release tarballs (mpg123 1.32.10, LAME 3.100), sha256-verifies
  them, and extracts only the needed subset into `third_party/{mpg123,lame}/`
  (gitignored). **Run `python3 initialize_files.py` once after cloning** — the
  cmake helpers hard-error with that instruction if the source is absent. Their
  autotools-generated `config.h` **is** committed, outside the downloaded tree in
  `third_party/{mpg123,lame}-config/android/`, and the build uses
  `cmake/ae_mpg123.cmake` / `ae_lame.cmake` (the `ae_libusb.cmake` pattern). The
  version pins + update steps live in `initialize_files.py`. Only libmpg123/
  libmp3lame are compiled — never the GPL CLI tools.
- **Phase 3 (parked)** — Windows WASAPI backend behind the same interfaces.

## Conventions

- C++17, CMake ≥ 3.22. Dependencies vendored or submodules; no package managers.
- Design docs live in `docs/superpowers/specs/`; plans in `~/.claude/plans/`.
- Commit only when asked; never commit `build/`, `.gradle/`, `.cxx/`, or
  `local.properties` (see `platform/android/.gitignore`).
