# backends/wasapi — Windows (parked, Phase 3)

Placeholder for the WASAPI `AudioSink`/`AudioSource` backends. Windows is not
built yet (see scripts/windows/build.ps1). USB already works on Windows via
libusb/WinUSB; WASAPI is the native shared-mode equivalent of the Linux
ALSA/JACK backends and will implement the same core interfaces.
