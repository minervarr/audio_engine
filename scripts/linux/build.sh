#!/usr/bin/env bash
# Desktop Linux build -> <repo>/build/linux
# Prereqs: cmake >= 3.22, ninja, a C++17 compiler, alsa-lib headers,
# and (optional, for the JACK backend) jack2 headers — NOT pipewire-jack.
set -euo pipefail
cd "$(dirname "$0")/../.."
# The whole desktop build is defined by the root CMakeLists.txt; this script is
# just a thin launcher (per-OS scripts live under scripts/, all source lives in
# core/ backends/ tools/).
cmake -S . -B build/linux -G Ninja "$@"
cmake --build build/linux
echo
echo "Binaries in build/linux/:"
echo "  USB:  list_usb_devices  capture_usb_to_wav"
echo "  ALSA: list_alsa_devices capture_alsa_to_wav  play_wav_to_alsa"
echo "  JACK: list_jack_ports   capture_jack_to_wav  play_wav_to_jack"
