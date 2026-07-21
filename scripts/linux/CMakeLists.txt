cmake_minimum_required(VERSION 3.22)
project("audio_engine_linux" C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Repo layout: engine core in <repo>/core, vendored libusb submodule in
# <repo>/third_party/libusb; this file lives in <repo>/scripts/linux.
set(AE_CORE        ${CMAKE_CURRENT_SOURCE_DIR}/../../core)
set(AE_THIRD_PARTY ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party)

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# libusb, Linux usbfs backend — the same four os/ files the Android build
# compiles (Android's kernel IS Linux/usbfs), plus a hand-rolled config.h
# under libusb_config/ modeled on third_party/libusb/android/config.h.
# ---------------------------------------------------------------------------
add_library(usb1 STATIC
    ${AE_THIRD_PARTY}/libusb/libusb/core.c
    ${AE_THIRD_PARTY}/libusb/libusb/descriptor.c
    ${AE_THIRD_PARTY}/libusb/libusb/hotplug.c
    ${AE_THIRD_PARTY}/libusb/libusb/io.c
    ${AE_THIRD_PARTY}/libusb/libusb/sync.c
    ${AE_THIRD_PARTY}/libusb/libusb/strerror.c
    ${AE_THIRD_PARTY}/libusb/libusb/os/linux_usbfs.c
    ${AE_THIRD_PARTY}/libusb/libusb/os/linux_netlink.c
    ${AE_THIRD_PARTY}/libusb/libusb/os/events_posix.c
    ${AE_THIRD_PARTY}/libusb/libusb/os/threads_posix.c)

target_include_directories(usb1 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/libusb_config
    ${AE_THIRD_PARTY}/libusb/libusb
    ${AE_THIRD_PARTY}/libusb/libusb/os)

target_include_directories(usb1 PUBLIC
    ${AE_THIRD_PARTY}/libusb/libusb)

target_compile_options(usb1 PRIVATE
    -Wno-unused-parameter
    -Wno-sign-compare)

target_link_libraries(usb1 PRIVATE Threads::Threads)

# ---------------------------------------------------------------------------
# audio_engine_linux — the cross-platform core (USB DAC/ADC driver) built
# for desktop Linux. Playback and capture, straight over libusb.
# ---------------------------------------------------------------------------
add_library(audio_engine_linux STATIC
    ${AE_CORE}/usb_audio.cpp)

target_include_directories(audio_engine_linux PUBLIC
    ${AE_CORE}
    ${AE_THIRD_PARTY}
    ${AE_THIRD_PARTY}/libusb/libusb)

target_link_libraries(audio_engine_linux
    PUBLIC usb1
    PRIVATE Threads::Threads)

# ---------------------------------------------------------------------------
# ALSA capture backend — direct hardware, no sound server.
# ---------------------------------------------------------------------------
find_package(ALSA REQUIRED)
add_library(audio_engine_alsa STATIC alsa_capture.cpp)
target_include_directories(audio_engine_alsa PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${AE_CORE})
target_link_libraries(audio_engine_alsa
    PUBLIC ALSA::ALSA
    PRIVATE Threads::Threads)

# ---------------------------------------------------------------------------
# Smoke-test tools (list devices / capture N seconds to WAV)
# ---------------------------------------------------------------------------
option(AUDIO_ENGINE_BUILD_TOOLS "Build capture smoke-test executables" ON)
if(AUDIO_ENGINE_BUILD_TOOLS)
    add_executable(list_usb_devices tools/list_usb_devices.cpp)
    target_link_libraries(list_usb_devices audio_engine_linux)

    add_executable(capture_usb_to_wav tools/capture_usb_to_wav.cpp)
    target_link_libraries(capture_usb_to_wav audio_engine_linux)

    add_executable(list_alsa_devices tools/list_alsa_devices.cpp)
    target_link_libraries(list_alsa_devices audio_engine_alsa)

    add_executable(capture_alsa_to_wav tools/capture_alsa_to_wav.cpp)
    target_link_libraries(capture_alsa_to_wav audio_engine_alsa)
endif()
