#!/usr/bin/env bash
# Desktop Linux build -> <repo>/build/linux
# Prereqs: cmake >= 3.22, ninja, a C++17 compiler, alsa-lib headers,
# and (optional, for the JACK backend) jack2 headers — NOT pipewire-jack.
set -euo pipefail
cd "$(dirname "$0")/../.."
cmake -S scripts/linux -B build/linux -G Ninja "$@"
cmake --build build/linux
echo
echo "Binaries in build/linux/: list_usb_devices capture_usb_to_wav" \
     "list_alsa_devices capture_alsa_to_wav list_jack_ports capture_jack_to_wav"
