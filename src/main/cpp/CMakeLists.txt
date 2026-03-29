cmake_minimum_required(VERSION 3.22.1)

project("matrix_audio")

# libusb as static library
add_library(usb1 STATIC
    libusb/libusb/core.c
    libusb/libusb/descriptor.c
    libusb/libusb/hotplug.c
    libusb/libusb/io.c
    libusb/libusb/sync.c
    libusb/libusb/strerror.c
    libusb/libusb/os/linux_usbfs.c
    libusb/libusb/os/linux_netlink.c
    libusb/libusb/os/events_posix.c
    libusb/libusb/os/threads_posix.c)

target_include_directories(usb1 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/libusb/libusb
    ${CMAKE_CURRENT_SOURCE_DIR}/libusb/libusb/os)

target_compile_options(usb1 PRIVATE
    -Wno-unused-parameter
    -Wno-sign-compare)

# Main shared library -- audio pipeline only
add_library(${CMAKE_PROJECT_NAME} SHARED
    usb_audio.cpp
    usb_audio_jni.cpp
    pcm_buffer_jni.cpp
    gapless_decoder_jni.cpp
    eq_processor_jni.cpp
    audio_convert_jni.cpp)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/libusb/libusb)

target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE
    -O3
    -funroll-loops)

target_link_libraries(${CMAKE_PROJECT_NAME}
    usb1
    android
    log)
