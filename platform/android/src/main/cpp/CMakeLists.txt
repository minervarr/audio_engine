cmake_minimum_required(VERSION 3.22.1)

project("matrix_audio")

# Repo layout: shared C++ core in <repo>/core, IO backends in <repo>/backends,
# vendored libusb in <repo>/third_party/libusb. This file sits five levels deep
# (platform/android/src/main/cpp), hence the ../ chain to the repo root.
set(AE_ROOT        ${CMAKE_CURRENT_SOURCE_DIR}/../../../../..)
set(AE_CORE        ${AE_ROOT}/core)
set(AE_BACKENDS    ${AE_ROOT}/backends)
set(AE_THIRD_PARTY ${AE_ROOT}/third_party)

# libusb static lib — one shared definition for every platform. On Android the
# helper selects the usbfs backend + the vendored android/config.h.
include(${AE_ROOT}/cmake/ae_libusb.cmake)
ae_add_libusb(usb1)

# Main shared library -- audio pipeline (core + USB backend) + JNI bridges.
add_library(${CMAKE_PROJECT_NAME} SHARED
    ${AE_BACKENDS}/usb/usb_audio.cpp
    usb_audio_jni.cpp
    pcm_buffer_jni.cpp
    gapless_decoder_jni.cpp
    eq_processor_jni.cpp
    audio_convert_jni.cpp)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${AE_CORE}/include
    ${AE_BACKENDS}/usb
    ${AE_THIRD_PARTY}
    ${AE_THIRD_PARTY}/libusb/libusb
    ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE
    -O3
    -funroll-loops)

target_link_libraries(${CMAKE_PROJECT_NAME}
    usb1
    android
    log)
