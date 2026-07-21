cmake_minimum_required(VERSION 3.22.1)

project("matrix_audio")

# Repo layout: cross-platform engine core lives in <repo>/core, vendored
# libusb submodule in <repo>/third_party/libusb. This file sits five levels
# deep (platform/android/src/main/cpp), hence the ../ chain.
set(AE_CORE        ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../core)
set(AE_THIRD_PARTY ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../third_party)

# libusb as static library (Android kernel is Linux -> usbfs backend)
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
    ${AE_THIRD_PARTY}
    ${AE_THIRD_PARTY}/libusb/android
    ${AE_THIRD_PARTY}/libusb/libusb
    ${AE_THIRD_PARTY}/libusb/libusb/os)

target_compile_options(usb1 PRIVATE
    -Wno-unused-parameter
    -Wno-sign-compare)

# Main shared library -- audio pipeline only
add_library(${CMAKE_PROJECT_NAME} SHARED
    ${AE_CORE}/usb_audio.cpp
    usb_audio_jni.cpp
    pcm_buffer_jni.cpp
    gapless_decoder_jni.cpp
    eq_processor_jni.cpp
    audio_convert_jni.cpp)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${AE_CORE}
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
