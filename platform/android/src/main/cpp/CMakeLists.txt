cmake_minimum_required(VERSION 3.22.1)

project("matrix_audio")

# Repo layout: shared C++ core in <repo>/core, IO backends in <repo>/backends,
# vendored libusb in <repo>/third_party/libusb. This file sits five levels deep
# (platform/android/src/main/cpp), hence the ../ chain to the repo root.
set(AE_ROOT        ${CMAKE_CURRENT_SOURCE_DIR}/../../../../..)
set(AE_CORE        ${AE_ROOT}/core)
set(AE_API         ${AE_ROOT}/api)
set(AE_BACKENDS    ${AE_ROOT}/backends)
set(AE_THIRD_PARTY ${AE_ROOT}/third_party)

# libusb static lib — one shared definition for every platform. On Android the
# helper selects the usbfs backend + the vendored android/config.h.
include(${AE_ROOT}/cmake/ae_libusb.cmake)
ae_add_libusb(usb1)

# Main shared library: 100% C++ (zero .java, per architectural directive 5).
# The pure-C++ engine (ae::Engine) + the C ABI wrapper + the native Android
# backends (AMediaCodec decode, AAudio output/capture, USB DAC, DSD). Every host
# app talks only to the C ABI in api/include/audio_engine.h.
add_library(${CMAKE_PROJECT_NAME} SHARED
    # engine core + C ABI
    ${AE_CORE}/src/engine.cpp
    ${AE_API}/src/audio_engine.cpp
    # native Android backends (decode + output + capture)
    ${AE_BACKENDS}/mediacodec/mediacodec_decoder.cpp
    ${AE_BACKENDS}/aaudio/aaudio_sink.cpp
    ${AE_BACKENDS}/aaudio/aaudio_source.cpp
    # USB backend (all platforms)
    ${AE_BACKENDS}/usb/usb_audio.cpp
    ${AE_BACKENDS}/usb/usb_sink.cpp
    # DSD backend (parsers + DoP packer; pure C++/POSIX, all platforms)
    ${AE_BACKENDS}/dsd/dff_parser.cpp
    ${AE_BACKENDS}/dsd/dsf_parser.cpp
    ${AE_BACKENDS}/dsd/dsd_decoder.cpp)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${AE_CORE}/include
    ${AE_API}/include
    ${AE_BACKENDS}/usb
    ${AE_BACKENDS}/mediacodec
    ${AE_BACKENDS}/aaudio
    ${AE_BACKENDS}/dsd
    ${AE_THIRD_PARTY}
    ${AE_THIRD_PARTY}/libusb/libusb
    ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE
    -O3
    -funroll-loops)

# aaudio: AAudio output/capture. mediandk: AMediaCodec/AMediaExtractor decode.
target_link_libraries(${CMAKE_PROJECT_NAME}
    usb1
    aaudio
    mediandk
    android
    log)
