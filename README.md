# Audio Engine

A high-fidelity Android audio playback library with USB DAC support, gapless playback, parametric EQ, and DSD format decoding. Written in Java + C++17 with JNI, targeting Android 7.0+.

No external dependencies. No UI. Pure engine.

---

## Table of Contents

1. [What It Does](#what-it-does)
2. [Architecture](#architecture)
3. [Integration](#integration)
4. [Quick Start](#quick-start)
5. [API Reference](#api-reference)
6. [Features Deep Dive](#features-deep-dive)
7. [Threading Model](#threading-model)
8. [Build Details](#build-details)
9. [Project Structure](#project-structure)

---

## What It Does

This library decodes and plays audio on Android with features that go beyond what `MediaPlayer` or `ExoPlayer` offer:

- **Gapless playback** -- pre-decodes the next track and trims encoder delay/padding for seamless transitions between songs.
- **USB DAC output** -- bypasses Android's audio mixer entirely and streams bit-perfect audio to USB Digital-to-Analog Converters via USB Audio Class 1.0 and 2.0 protocols.
- **Parametric EQ** -- up to 10 cascaded biquad filters (peaking, low shelf, high shelf) with NEON SIMD acceleration on ARM64. Hundreds of built-in profiles included.
- **DSD support** -- parses and plays Sony DSF and Philips DSDIFF files natively over USB.
- **Format conversion** -- float32-to-int16 with TPDF dither, int16-to-int24 upscaling, and other conversions handled automatically based on output capabilities.

### Supported Input Formats

Anything Android's `MediaCodec` can decode: FLAC, MP3, AAC, WAV, OGG Vorbis, Opus, ALAC, plus DSD files (.dsf, .dff) via built-in parsers.

### Supported Output Paths

| Output | Class | Description |
|--------|-------|-------------|
| Speaker / Bluetooth | `AudioTrackOutput` | Routes through Android's `AudioTrack` API. Float decoded audio is downconverted to 16-bit with TPDF dither. |
| USB DAC | `UsbAudioOutput` | Direct isochronous USB streaming via libusb. Supports 16/24/32-bit PCM and DSD. Bit-perfect when format matches. |
| Custom | Implement `AudioOutput` | You can implement the 8-method interface and route audio anywhere. |

---

## Architecture

```
+------------------------------------------------------------+
|                      Your Application                       |
+------------------------------------------------------------+
        |                                     |
        v                                     v
+------------------+              +------------------------+
|  MatrixPlayer    |              |    AudioEngine         |
|  (simple facade) |              |    (full control)      |
+------------------+              +------------------------+
        |                                     |
        +------------------+------------------+
                           |
                    Java / JNI boundary
                           |
        +------------------+------------------+
        |                  |                  |
+---------------+ +----------------+ +----------------+
| pcm_buffer.h  | | eq_processor.h | | usb_audio.h    |
| Ring buffer    | | Biquad EQ      | | USB driver     |
| (mutex + CV)   | | (NEON SIMD)    | | (libusb)       |
+---------------+ +----------------+ +----------------+
        |                  |                  |
+---------------+ +----------------+ +----------------+
| gapless_      | | audio_         | | libusb/        |
| decoder.h     | | convert.h      | | (vendored)     |
| Delay/padding | | Float->Int16   | |                |
| trimming      | | TPDF dither    | |                |
+---------------+ +----------------+ +----------------+
```

### Data Flow

```
Audio source (file URI or MediaDataSource stream)
     |
     v
MediaExtractor (demux container)
     |
     +---> MediaCodec (decode FLAC/MP3/AAC/etc. to PCM)
     |          or
     +---> DsfParser / DffParser (extract DSD blocks)
     |
     v
NativeGaplessDecoder (trim encoder delay at start, padding at end)
     |
     v
NativePcmBuffer (1MB ring buffer, decode thread -> output thread)
     |
     v
EqProcessor (optional, biquad filters applied in-place)
     |
     v
AudioOutput implementation
     |
     +---> AudioTrackOutput (speaker): float32 -> int16 with dither
     +---> UsbAudioOutput (USB DAC): bit-perfect or format-converted
     +---> Your custom output
```

---

## Integration

### As a Gradle Module

In your project's `settings.gradle`:

```groovy
include ':audio-engine'
project(':audio-engine').projectDir = file('/path/to/audio-engine')
```

In your app's `build.gradle`:

```groovy
dependencies {
    implementation project(':audio-engine')
}
```

### Requirements

| Requirement | Value |
|-------------|-------|
| Min SDK | 24 (Android 7.0) |
| Compile SDK | 36 |
| Java | 11 |
| C++ | 17 |
| CMake | 3.22.1 |
| External dependencies | None |

### Permissions

The library declares no permissions. Your app needs:

- **USB DAC**: `android.permission.USB_PERMISSION` (requested at runtime via `UsbManager`)
- **Streaming**: `android.permission.INTERNET` (if playing from network sources)
- **Local files**: Storage permissions as needed by your app's target SDK

### ProGuard / R8

The library ships with a consumer ProGuard rule that keeps all classes:

```
-keep class com.nerio.audioengine.** { *; }
```

This is required because the native C++ code calls Java methods via JNI reflection.

---

## Quick Start

### Simple Playback with MatrixPlayer

`MatrixPlayer` is a facade that handles EQ lifecycle and provides a simple API. This is the recommended entry point for most use cases.

```java
import com.nerio.audioengine.MatrixPlayer;
import com.nerio.audioengine.AudioTrackOutput;

// Create player (defaults to speaker output)
MatrixPlayer player = new MatrixPlayer();

// Set listeners
player.setOnPreparedListener(p -> {
    Log.d("Player", "Ready: " + p.getSampleRate() + "Hz, "
            + p.getSourceBitDepth() + "-bit");
});

player.setOnCompletionListener(p -> {
    Log.d("Player", "Track finished");
});

player.setOnErrorListener((p, message) -> {
    Log.e("Player", "Error: " + message);
});

// Play a file
player.play(context, Uri.fromFile(new File("/sdcard/Music/song.flac")));

// Control playback
player.pause();
player.resume();
player.togglePlayPause();
player.seekTo(30000); // seek to 30 seconds

// Query state
int position = player.getCurrentPosition(); // milliseconds
int duration = player.getDuration();         // milliseconds
boolean playing = player.isPlaying();

// Clean up
player.stop();
player.release();
```

### Gapless Playback (Queue Next Track)

```java
// While current track is playing, queue the next one
player.queueNext(context, Uri.fromFile(new File("/sdcard/Music/track02.flac")));

// Listen for the seamless transition
player.setOnTrackTransitionListener(p -> {
    Log.d("Player", "Transitioned to next track");
    // Queue another track for continuous gapless playback
    player.queueNext(context, nextTrackUri);
});

// Cancel queued track if needed
player.cancelNext();
```

### USB DAC Output

```java
import com.nerio.audioengine.UsbAudioOutput;

// Get USB device file descriptor from Android's UsbManager
UsbDeviceConnection connection = usbManager.openDevice(device);
int fd = connection.getFileDescriptor();

// Create USB output and switch to it
UsbAudioOutput usbOutput = new UsbAudioOutput(fd);
player.switchOutput(usbOutput);

// Query DAC capabilities
int[] rates = usbOutput.getSupportedRates();  // e.g., [44100, 48000, 96000, 192000]
int uacVersion = usbOutput.getUacVersion();   // 0x0100 or 0x0200
String info = usbOutput.getDeviceInfo();       // device description string

// Switch back to speaker
player.switchOutput(new AudioTrackOutput());
```

### Parametric EQ

```java
import com.nerio.audioengine.EqProfile;

// Load built-in profiles (hundreds available, cached after first load)
List<EqProfile> profiles = MatrixPlayer.loadEqProfiles(context);

// Apply a profile
player.setEqProfile(profiles.get(0));

// Disable EQ
player.setEqProfile(null);

// Find a specific profile by name
EqProfile profile = EqProfile.find(context, "HD 600", "oratory1990", "over-ear");
if (profile != null) {
    player.setEqProfile(profile);
}
```

### Lower-Level AudioEngine Usage

For more control, use `AudioEngine` directly:

```java
import com.nerio.audioengine.AudioEngine;
import com.nerio.audioengine.AudioTrackOutput;
import com.nerio.audioengine.EqProcessor;

AudioEngine engine = new AudioEngine();
engine.switchOutput(new AudioTrackOutput());

engine.setOnPreparedListener(e -> {
    Log.d("Engine", "Format: " + e.getMime() + " " + e.getSampleRate() + "Hz "
            + e.getSourceBitDepth() + "-bit " + e.getChannelCount() + "ch");
    Log.d("Engine", "Codec: " + e.getCodecName());
});

engine.setOnCompletionListener(e -> { /* track done */ });
engine.setOnErrorListener((e, msg) -> { /* handle error */ });
engine.setOnTransitionListener(e -> { /* gapless transition happened */ });

// Direct EQ control
EqProcessor eq = new EqProcessor();
engine.setEqProcessor(eq);
// You must call computeCoefficients yourself when sample rate is known
eq.computeCoefficients(profile, engine.getSampleRate(),
        engine.getChannelCount(), engine.getEncoding());
eq.setEnabled(true);

engine.play(context, uri);

// Hot-swap output while playing
engine.switchOutput(new UsbAudioOutput(fd));

// Access underlying output
AudioOutput currentOutput = engine.getOutput();
```

### Signal Path Info

Get complete metadata about the current audio signal chain:

```java
SignalPathInfo info = player.getSignalPathInfo();

// Source info
info.sourceRate;       // 96000
info.sourceBitDepth;   // 24
info.sourceMime;       // "audio/flac"
info.isDsd;            // false
info.dsdRate;          // 0 (or 2822400 for DSD64)

// Decode info
info.codecName;        // "c2.android.flac.decoder"
info.decodedEncoding;  // AudioFormat.ENCODING_PCM_FLOAT

// EQ info
info.eqActive;         // true
info.eqProfileName;    // "HD 600"

// Quality indicators (colors for UI)
int srcColor = info.getSourceQualityColor();   // green for lossless, amber for lossy
int decColor = info.getDecodeQualityColor();   // green for native, amber for converted
int outColor = info.getOutputQualityColor();   // green for USB bit-perfect
```

---

## API Reference

### MatrixPlayer

The recommended entry point. Wraps `AudioEngine` and manages EQ lifecycle automatically.

```java
public class MatrixPlayer {
    // Construction
    MatrixPlayer()                    // Creates engine with AudioTrackOutput (speaker)

    // Playback
    void play(Context, Uri)           // Play from file/content URI
    void playStream(MediaDataSource, long durationHintUs)  // Play from stream
    void pause()
    void resume()
    void togglePlayPause()
    void seekTo(int positionMs)
    void stop()
    void release()                    // Frees all resources (engine + EQ)

    // Gapless
    void queueNext(Context, Uri)      // Queue next track for gapless transition
    void queueNextStream(MediaDataSource, long durationHintUs)
    void cancelNext()

    // Output
    boolean switchOutput(AudioOutput)

    // EQ
    void setEqProfile(EqProfile)      // null disables EQ
    static List<EqProfile> loadEqProfiles(Context)

    // State
    boolean isPlaying()
    int getCurrentPosition()          // milliseconds
    int getDuration()                 // milliseconds
    int getSampleRate()               // Hz
    int getChannelCount()
    int getEncoding()                 // AudioFormat.ENCODING_PCM_*
    int getSourceBitDepth()           // 16, 24, 32, or 1 for DSD
    boolean isDsd()
    String getMime()                  // "audio/flac", "audio/mpeg", etc.
    String getCodecName()             // "c2.android.flac.decoder", etc.
    SignalPathInfo getSignalPathInfo()

    // Advanced
    AudioEngine getEngine()           // Direct engine access

    // Listeners
    void setOnPreparedListener(OnPreparedListener)
    void setOnCompletionListener(OnCompletionListener)
    void setOnErrorListener(OnErrorListener)
    void setOnTrackTransitionListener(OnTrackTransitionListener)
}
```

### AudioEngine

The core engine. Use this when you need full control over the playback pipeline.

```java
public class AudioEngine {
    // Playback (same as MatrixPlayer)
    void play(Context, Uri)
    void playStream(MediaDataSource, long durationHintUs)
    void pause()
    void resume()
    void seekTo(int positionMs)
    void stop()
    void release()

    // Gapless
    void queueNext(Context, Uri)
    void queueNextStream(MediaDataSource, long durationHintUs)
    void cancelNext()

    // Output
    boolean switchOutput(AudioOutput)
    AudioOutput getOutput()

    // EQ (manual management)
    void setEqProcessor(EqProcessor)
    EqProcessor getEqProcessor()

    // State (same as MatrixPlayer)
    boolean isPlaying()
    int getCurrentPosition()
    int getDuration()
    int getSampleRate()
    int getChannelCount()
    int getEncoding()
    int getSourceBitDepth()
    boolean isDsd()
    int getDsdRate()                  // 2822400, 5644800, or 11289600
    String getMime()
    String getCodecName()

    // Listeners
    void setOnPreparedListener(OnPreparedListener)
    void setOnCompletionListener(OnCompletionListener)
    void setOnErrorListener(OnErrorListener)
    void setOnTransitionListener(OnTransitionListener)
}
```

**Key difference from MatrixPlayer**: when using `AudioEngine` directly, you manage the `EqProcessor` yourself. You must call `eq.computeCoefficients(...)` when the sample rate becomes known (in `onPrepared`) and again on track transitions (in `onTransition`). `MatrixPlayer` does this automatically.

### AudioOutput (Interface)

Implement this to create a custom audio output.

```java
public interface AudioOutput {
    boolean configure(int sampleRate, int channelCount, int encoding, int sourceBitDepth);
    boolean start();
    int write(byte[] data, int offset, int length);  // returns bytes consumed
    void pause();
    void resume();
    void flush();
    void stop();
    void release();
}
```

The engine calls these methods in order: `configure` -> `start` -> `write` (repeated) -> `stop` -> `release`. `pause`/`resume` and `flush` can be called between `start` and `stop`. The `write` method receives raw PCM bytes and must return the number of bytes consumed.

### AudioTrackOutput

Speaker output via Android's `AudioTrack` API.

- Automatically downgrades `PCM_FLOAT` to `PCM_16BIT` with TPDF dither (not all hardware handles float)
- Creates `AudioTrack` with 4x minimum buffer size
- Does not support DSD (returns `false` from `configure`)

```java
AudioTrackOutput output = new AudioTrackOutput();
```

### UsbAudioOutput

USB DAC output via native libusb driver.

```java
public class UsbAudioOutput implements AudioOutput {
    UsbAudioOutput(int fd)            // USB device file descriptor

    boolean open()                    // Open native driver (called lazily)
    int getConfiguredBitDepth()       // DAC's actual bit depth
    boolean isDsdMode()               // True if streaming DSD
    int getUacVersion()               // 0x0100 (UAC1) or 0x0200 (UAC2)
    String getDeviceInfo()            // Device description
    long getNativeHandle()            // For advanced native access
    int[] getSupportedRates()         // e.g., [44100, 48000, 96000, 192000]
}
```

Write path logic:
- **Float32 input**: converted to DAC's native bit depth in C++
- **Int16 input, DAC > 16-bit**: upscaled in C++ to DAC bit depth
- **Matching bit depth**: raw passthrough (bit-perfect)
- **DSD**: streams 32-bit DSD containers directly

### EqProcessor

Native biquad equalizer with NEON SIMD acceleration.

```java
public class EqProcessor {
    EqProcessor()

    void computeCoefficients(EqProfile profile, int sampleRate,
                             int channelCount, int encoding)
    void process(byte[] data, int offset, int length)  // in-place
    void reset()                      // Clear filter state (call after seek)
    void setEnabled(boolean enabled)
    void destroy()                    // Free native resources
    EqProfile getCurrentProfile()
}
```

Coefficients are computed in Java using Robert Bristow-Johnson's Audio EQ Cookbook formulas. The native C++ side applies the filters using Transposed Direct Form II biquads. On ARM64, stereo float/int16/int32 processing uses NEON SIMD for ~4x throughput.

### EqProfile

EQ profile data and loader.

```java
public class EqProfile {
    String name;                      // "HD 600"
    String source;                    // "oratory1990"
    String form;                      // "over-ear"
    double preamp;                    // dB (typically negative to prevent clipping)
    List<Filter> filters;

    static class Filter {
        String type;                  // "PK" (peaking), "LSC" (low shelf), "HSC" (high shelf)
        double fc;                    // center frequency in Hz
        double gain;                  // dB
        double q;                     // Q factor
    }

    static synchronized List<EqProfile> loadAll(Context)
    static EqProfile find(Context, String name, String source, String form)
}
```

Profiles are stored in `assets/eq_profiles.bin` (gzip-compressed JSON, ~634KB compressed, ~4.8MB uncompressed). Loaded once and cached in memory.

### DsfParser / DffParser

DSD file format parsers. Used internally by `AudioEngine` but available for direct use.

```java
// DSF (Sony format, little-endian, planar blocks, LSB-first)
DsfParser dsf = new DsfParser();
dsf.parse(randomAccessFile);
dsf.getSampleRate();         // 2822400 (DSD64), 5644800 (DSD128), etc.
dsf.getChannelCount();       // 2
dsf.getTotalBlocks();
dsf.readBlockPair(leftBlock, rightBlock);  // returns false at EOF
dsf.seekToBlock(blockIndex);

// DFF (Philips format, big-endian, interleaved, MSB-first)
DffParser dff = new DffParser();
dff.parse(randomAccessFile);
// Same API as DsfParser
```

### SignalPathInfo

Data class describing the complete audio signal chain. All fields are public.

| Category | Fields |
|----------|--------|
| **Source** | `sourceFormat`, `sourceRate`, `sourceBitDepth`, `sourceChannels`, `sourceMime` |
| **Decode** | `codecName`, `decodedEncoding`, `isDsd`, `dsdRate`, `dsdPcmRate`, `dsdPlaybackMode` |
| **Output** | `outputDevice`, `outputRate`, `outputBitDepth`, `outputChannels`, `uacVersion`, `usbDeviceInfo`, `isBitPerfect` |
| **EQ** | `eqActive`, `eqProfileName` |

Quality color methods for UI: `getSourceQualityColor()`, `getDecodeQualityColor()`, `getOutputQualityColor()` return ARGB int colors (green = lossless/bit-perfect, amber = lossy/converted).

---

## Features Deep Dive

### Gapless Playback

The engine achieves true gapless playback through two mechanisms:

**1. Encoder delay/padding trimming**

MP3 and AAC encoders add silence at the beginning (encoder delay) and end (padding) of the audio stream. The engine reads this metadata from the container and uses `NativeGaplessDecoder` in C++ to:
- Skip the first N frames (delay) during decode
- Hold the last N frames in a tail buffer and discard them at end-of-stream (padding)

This eliminates the brief silence that would otherwise occur between tracks.

**2. Pre-decode pipeline**

When you call `queueNext()`, the engine:
1. Creates a second `MediaExtractor` + `MediaCodec` + `NativePcmBuffer` in the background
2. Starts a `nextDecodeThread` that pre-decodes the next track into `nextPcmBuffer`
3. When the current track's buffer returns EOF (-1), the output thread:
   - Swaps `pcmBuffer` with `nextPcmBuffer`
   - Swaps codec pipelines
   - Reconfigures the output if the format changed (different sample rate, etc.)
   - Resets EQ filter state
   - Fires `onTransitionListener`
4. Playback continues without any gap

### USB DAC Output

The native USB audio driver (`usb_audio.h`/`.cpp`) implements:

- **UAC 1.0 and 2.0 descriptor parsing**: automatically detects supported sample rates, bit depths, and channel counts from the device
- **Isochronous USB transfers**: 8 concurrent transfers with 8 packets each for low-latency streaming
- **Feedback endpoint**: reads the DAC's feedback endpoint to adjust sample delivery rate, preventing buffer underrun/overrun
- **Page-locked memory**: uses `mlock()` to prevent page faults during real-time streaming
- **Ring buffer**: lock-free atomic ring buffer between the Java write path and USB transfer callbacks
- **Kernel driver detach**: automatically detaches the kernel driver to take exclusive control

Supported configurations:
- Sample rates: typically 44.1kHz - 384kHz (depends on DAC)
- Bit depths: 16, 24, 32-bit PCM
- DSD over USB (native, not DoP)

### Parametric EQ

The EQ processor supports up to 10 cascaded biquad filters per profile:

| Filter Type | Code | Description |
|------------|------|-------------|
| Peaking | `PK` | Boost or cut a frequency band with adjustable Q |
| Low Shelf | `LSC` | Boost or cut all frequencies below a threshold |
| High Shelf | `HSC` | Boost or cut all frequencies above a threshold |

Each filter has: center frequency (Hz), gain (dB), and Q factor.

The processing pipeline:
1. **Java**: computes 5 biquad coefficients [b0, b1, b2, a1, a2] per filter using Bristow-Johnson formulas
2. **C++ (NEON)**: applies filters in Transposed Direct Form II with double-precision state variables
3. **Per-channel**: each audio channel has independent filter state (up to 8 channels)

Supported PCM formats: `ENCODING_PCM_FLOAT`, `ENCODING_PCM_16BIT`, `ENCODING_PCM_24BIT_PACKED`, `ENCODING_PCM_32BIT`.

The included `eq_profiles.bin` asset contains hundreds of headphone correction profiles from sources like oratory1990 and crinacle.

### DSD Support

Direct Stream Digital files are parsed and streamed without PCM conversion when using USB output:

- **DSF** (Sony): planar per-channel blocks, LSB-first (requires bit-reversal via lookup table)
- **DFF/DSDIFF** (Philips): interleaved byte pairs, MSB-first (no bit-reversal needed)

DSD rates: DSD64 (2.8224 MHz), DSD128 (5.6448 MHz), DSD256 (11.2896 MHz).

DSD audio is packed into 32-bit containers and streamed to the USB DAC. If the output doesn't support DSD, it is not played (DSD-to-PCM conversion is not implemented in this library).

### Audio Format Conversion

The `audio_convert.h` module handles float32 to int16 conversion with TPDF (Triangular Probability Density Function) dither:

- **NEON SIMD**: processes 4 samples per iteration on ARM64
- **LCG-based dither**: fast linear congruential generator (no system entropy calls)
- **TPDF**: sum of two uniform random values produces triangular distribution, which optimally masks quantization noise

This conversion is used automatically by `AudioTrackOutput` since Android's speaker path works best with 16-bit PCM.

---

## Threading Model

The engine uses dedicated threads for decoding and output to prevent audio glitches:

```
Main Thread
  |
  +--> play() / stop() / seekTo() / queueNext()
  |
  |    Decode Thread
  |      MediaExtractor.readSampleData()
  |      MediaCodec.dequeueInputBuffer() / queueInputBuffer()
  |      MediaCodec.dequeueOutputBuffer()
  |      NativeGaplessDecoder.processFrame()
  |      NativePcmBuffer.write()      --+
  |                                     |  (1MB ring buffer)
  |    Output Thread                    |
  |      [THREAD_PRIORITY_URGENT_AUDIO] |
  |      NativePcmBuffer.read()      <--+
  |      EqProcessor.process()
  |      AudioOutput.write()
  |
  |    Next Decode Thread (gapless)
  |      Pre-decodes next track into nextPcmBuffer
  |
  |    Seek Executor (single-thread pool)
  |      Handles async seek for network streams
```

### Synchronization

| Lock | Purpose |
|------|---------|
| `outputLock` | Guards `AudioOutput` access -- prevents concurrent writes during output switching |
| `pauseLock` | Guards the `playing` flag -- pause/resume wake blocked decode and output threads |
| `codecLock` | Guards `MediaCodec` and `MediaExtractor` access during seeks |

The `NativePcmBuffer` uses its own internal mutex + condition variables with 100ms timeouts. The decode thread blocks on write when the buffer is full; the output thread blocks on read when the buffer is empty. Flush and end-of-stream signals wake all blocked threads.

---

## Build Details

### Native Libraries

The library compiles two native targets:

1. **`libusb1.a`** (static) -- vendored libusb compiled from source with Linux usbfs backend
2. **`libmatrix_audio.so`** (shared) -- the JNI bridge library containing all native code

Compiler flags: `-std=c++17 -O3 -funroll-loops`

The shared library loads automatically via `System.loadLibrary("matrix_audio")` in `EqProcessor`'s static initializer.

### Dependencies

None. The library uses only:
- Android SDK (MediaCodec, MediaExtractor, AudioTrack, AudioFormat)
- Android NDK (JNI, NEON intrinsics)
- Vendored libusb (compiled from source)
- `org.json` (bundled with Android)

---

## Project Structure

```
audio-engine/
|-- build.gradle                    Gradle build config (Android library module)
|-- proguard-rules.pro              Keep rules for JNI
|-- src/main/
    |-- AndroidManifest.xml         Empty manifest (library)
    |-- assets/
    |   |-- eq_profiles.bin         Gzip JSON, hundreds of EQ profiles (~634KB)
    |-- cpp/
    |   |-- CMakeLists.txt          Native build config
    |   |-- usb_audio.h             USB Audio Class driver (UAC1/UAC2)
    |   |-- usb_audio.cpp           USB driver implementation (1039 lines)
    |   |-- usb_audio_jni.cpp       JNI bridge for USB
    |   |-- eq_processor.h          Biquad EQ with NEON SIMD (314 lines)
    |   |-- eq_processor_jni.cpp    JNI bridge for EQ
    |   |-- pcm_buffer.h            Ring buffer with mutex + CV (126 lines)
    |   |-- pcm_buffer_jni.cpp      JNI bridge for ring buffer
    |   |-- gapless_decoder.h       Encoder delay/padding trimmer (82 lines)
    |   |-- gapless_decoder_jni.cpp JNI bridge for gapless
    |   |-- audio_convert.h         Float->Int16 with TPDF dither (86 lines)
    |   |-- audio_convert_jni.cpp   JNI bridge for conversion
    |   |-- libusb/                 Vendored libusb source tree
    |-- java/com/nerio/audioengine/
        |-- MatrixPlayer.java       Simple facade (recommended entry point)
        |-- AudioEngine.java        Core engine (1570 lines)
        |-- AudioOutput.java        Output interface (8 methods)
        |-- AudioTrackOutput.java   Speaker output
        |-- UsbAudioOutput.java     USB DAC output
        |-- UsbAudioNative.java     JNI declarations for USB
        |-- NativePcmBuffer.java    Ring buffer JNI wrapper
        |-- NativeGaplessDecoder.java  Gapless trim JNI wrapper
        |-- EqProcessor.java        EQ processor (biquad coefficient computation)
        |-- EqProfile.java          EQ profile data + asset loader
        |-- DsfParser.java          Sony DSF parser
        |-- DffParser.java          Philips DSDIFF parser
        |-- SignalPathInfo.java      Signal chain metadata
```
