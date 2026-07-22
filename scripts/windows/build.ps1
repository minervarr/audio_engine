# Desktop Windows build launcher (DORMANT — Phase 3).
#
# Windows is parked: the WASAPI backends under backends/wasapi/ are not
# implemented yet, so this configures the root CMakeLists.txt to build only the
# shared core + the cross-platform USB backend (via WinUSB/libusbK). The
# ALSA/JACK backends and the smoke tools are Linux-only and won't be built here.
#
# Prereqs when unparked: cmake >= 3.22, ninja, and the MSVC toolchain
# (run from a "x64 Native Tools" prompt, or let CMake locate MSVC).
#
#   scripts\windows\build.ps1              # configure + build
#   scripts\windows\build.ps1 -D FOO=BAR   # extra CMake args passed through

param([Parameter(ValueFromRemainingArguments = $true)][string[]]$CMakeArgs)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
cmake -S "$root" -B "$root\build\windows" -G Ninja @CMakeArgs
cmake --build "$root\build\windows"
