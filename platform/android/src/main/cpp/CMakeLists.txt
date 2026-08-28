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

# 32-bit Android (armeabi-v7a) quirks in the vendored decoders — see the long
# comment in the repo-root CMakeLists.txt (this project repeats it in brief):
# HAVE_FSEEKO stops libFLAC's compat.h from `#define fseeko fseek` into
# bionic's real API-24 fseeko (bionic has had it since 24; our floor is 26),
# and _FILE_OFFSET_BITS=64 gives 32-bit off_t the 8 bytes mpg123's committed
# android config.h promises. Both are no-ops on 64-bit ABIs.
add_compile_definitions(_FILE_OFFSET_BITS=64 HAVE_FSEEKO)

# libusb static lib — one shared definition for every platform. On Android the
# helper selects the usbfs backend + the vendored android/config.h.
include(${AE_ROOT}/cmake/ae_libusb.cmake)
ae_add_libusb(usb1)

# libmpg123 (LGPL-2.1) native MP3 decoder + LAME/libmp3lame (LGPL) MP3 encoder.
# Both are autotools libs with no CMake, built via our helpers against a
# build-generated config.h kept outside the pristine third_party trees (same
# pattern as ae_libusb). Native MP3 = identical on every OEM, unlike AMediaCodec.
include(${AE_ROOT}/cmake/ae_mpg123.cmake)
ae_add_mpg123(mpg123)
include(${AE_ROOT}/cmake/ae_lame.cmake)
ae_add_lame(mp3lame)

# libFLAC static lib (BSD-3-Clause; decoder + encoder) for native FLAC playback
# and recording — identical on every OEM, unlike AMediaCodec. Build ONLY the
# library: no CLI tools (those are GPL), no C++ wrapper/tests/docs, no Ogg.
# The 32-bit fseeko/ftello quirk is handled by HAVE_FSEEKO above; libFLAC stays
# pristine (configured via CMake cache vars and compile definitions, never patched).
set(BUILD_CXXLIBS             OFF CACHE BOOL "" FORCE)
set(BUILD_PROGRAMS            OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES            OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING             OFF CACHE BOOL "" FORCE)
set(BUILD_DOCS                OFF CACHE BOOL "" FORCE)
set(WITH_OGG                  OFF CACHE BOOL "" FORCE)
set(WITH_STACK_PROTECTOR      OFF CACHE BOOL "" FORCE)
set(INSTALL_MANPAGES          OFF CACHE BOOL "" FORCE)
set(INSTALL_PKGCONFIG_MODULES OFF CACHE BOOL "" FORCE)
set(INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS         OFF CACHE BOOL "" FORCE)
add_subdirectory(${AE_THIRD_PARTY}/flac ${CMAKE_BINARY_DIR}/flac_build)

# Main shared library: 100% C++ (zero .java, per architectural directive 5).
# The pure-C++ engine (ae::Engine) + the C ABI wrapper + the native Android
# backends (AMediaCodec decode, AAudio output/capture, USB DAC, DSD). Every host
# app talks only to the C ABI in api/include/audio_engine.h.
add_library(${CMAKE_PROJECT_NAME} SHARED
    # engine core + C ABI
    ${AE_CORE}/src/engine.cpp
    ${AE_CORE}/src/recorder.cpp
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
    ${AE_BACKENDS}/dsd/dsd_decoder.cpp
    # FLAC backend (native decode + encode via vendored libFLAC)
    ${AE_BACKENDS}/flac/flac_decoder.cpp
    ${AE_BACKENDS}/flac/flac_encoder.cpp
    # MP3 backend (native decode via libmpg123 + encode via LAME)
    ${AE_BACKENDS}/mp3/mp3_decoder.cpp
    ${AE_BACKENDS}/mp3/mp3_encoder.cpp)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${AE_CORE}/include
    ${AE_API}/include
    ${AE_BACKENDS}/usb
    ${AE_BACKENDS}/mediacodec
    ${AE_BACKENDS}/aaudio
    ${AE_BACKENDS}/dsd
    ${AE_BACKENDS}/flac
    ${AE_BACKENDS}/mp3
    ${AE_THIRD_PARTY}
    ${AE_THIRD_PARTY}/libusb/libusb
    ${AE_THIRD_PARTY}/flac/include
    ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE
    -O3
    -funroll-loops)

# aaudio: AAudio output/capture. mediandk: AMediaCodec/AMediaExtractor decode.
target_link_libraries(${CMAKE_PROJECT_NAME}
    usb1
    FLAC
    mpg123
    mp3lame
    aaudio
    mediandk
    android
    log)
