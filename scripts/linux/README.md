# Desktop Linux build

`scripts/` holds only the build launcher — all source lives in `core/`,
`backends/`, and `tools/`, and the whole desktop build is defined by the repo
root `CMakeLists.txt`.

```
scripts/linux/build.sh      # configures + builds -> <repo>/build/linux
```

Prerequisites: `cmake` ≥ 3.22, `ninja`, a C++17 compiler, `alsa-lib`
headers, and — for the JACK backend — `jack2` dev headers. **Do not build
against pipewire-jack**; after building, verify with:

```
ldd ../../build/linux/capture_jack_to_wav | grep jack
# must resolve to jack2's libjack.so.0, nothing containing "pipewire"
```

## Backends (full-duplex)

Each backend implements the shared core interfaces: `ae::AudioSource` for
capture (`configure -> start -> read -> stop`) and `ae::AudioSink` for playback
(`configure -> start -> write -> stop`).

| Backend | Source files | Path to the hardware |
|---|---|---|
| USB DAC/ADC | `backends/usb/usb_audio.*` (+ `usb_io.h` adapters) | libusb straight to the device (UAC1/UAC2), bypassing every sound layer |
| ALSA | `backends/alsa/alsa_source.*`, `alsa_sink.*` | direct `hw:` device access, no sound server (Audacity-style) |
| JACK2 | `backends/jack/jack_source.*`, `jack_sink.*` | client of the **running** `jackd` you started (e.g. via qjackctl); never auto-spawns a server |

## Smoke tools

```
# list / capture (AudioSource)
list_usb_devices                              # UAC devices via libusb
capture_usb_to_wav  <vid> <pid> <s> <out.wav>
list_alsa_devices                             # capture-capable hw:N,D
capture_alsa_to_wav <hw:N,D> <s> <out.wav>
list_jack_ports                               # needs jackd running
capture_jack_to_wav <s> <out.wav> [channels]

# play (AudioSink) — integer-PCM WAV in
play_wav_to_alsa    <hw:N,D> <in.wav>
play_wav_to_jack    <in.wav>                  # needs jackd running
```

Note: ALSA `hw:` access and a running jackd are mutually exclusive on the
same card — while jackd holds the device, use the JACK backend.

USB capture/playback may need permissions: either run once with sudo to
confirm, or install a udev rule granting your user access to the DAC's vid/pid.
