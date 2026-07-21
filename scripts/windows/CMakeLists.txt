cmake_minimum_required(VERSION 3.22.1)
project("audio_engine_windows")

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Repo layout: engine core in <repo>/core, vendored libusb submodule in
# <repo>/third_party/libusb; this file lives in <repo>/scripts/windows.
set(AE_CORE        ${CMAKE_CURRENT_SOURCE_DIR}/../../core)
set(AE_THIRD_PARTY ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party)

# libusb Windows static library
add_library(usb1 STATIC
    ${AE_THIRD_PARTY}/libusb/libusb/core.c
    ${AE_THIRD_PARTY}/libusb/libusb/descriptor.c
    ${AE_THIRD_PARTY}/libusb/libusb/hotplug.c
    ${AE_THIRD_PARTY}/libusb/libusb/io.c
    ${AE_THIRD_PARTY}/libusb/libusb/sync.c
    ${AE_THIRD_PARTY}/libusb/libusb/strerror.c
    ${AE_THIRD_PARTY}/libusb/libusb/os/windows_winusb.c
    ${AE_THIRD_PARTY}/libusb/libusb/os/windows_usbdk.c
    ${AE_THIRD_PARTY}/libusb/libusb/os/windows_common.c
    ${AE_THIRD_PARTY}/libusb/libusb/os/events_windows.c
    ${AE_THIRD_PARTY}/libusb/libusb/os/threads_windows.c)

target_include_directories(usb1 PUBLIC
    ${AE_THIRD_PARTY}/libusb/libusb)

target_include_directories(usb1 PRIVATE
    ${AE_THIRD_PARTY}/libusb/libusb/os
    ${AE_THIRD_PARTY}/libusb/msvc)

target_compile_options(usb1 PRIVATE /W0)

# audio_engine_windows static library
add_library(audio_engine_windows STATIC
    ${AE_CORE}/usb_audio.cpp)

target_include_directories(audio_engine_windows PUBLIC
    ${AE_CORE}
    ${AE_THIRD_PARTY}
    ${AE_THIRD_PARTY}/libusb/libusb)

# No hardcoded /O2: CMAKE_BUILD_TYPE already selects the right optimization
# level per config. Forcing /O2 unconditionally breaks a Debug build outright
# here — MSVC's default Debug flags include /RTC1 (runtime checks), and cl.exe
# fatal-errors (D8016) on the combination "/RTC1 and /O2 are incompatible"
# rather than silently picking one.
target_compile_options(audio_engine_windows PRIVATE /W3)

target_link_libraries(audio_engine_windows PRIVATE usb1)
