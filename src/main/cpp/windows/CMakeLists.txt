cmake_minimum_required(VERSION 3.22.1)
project("audio_engine_windows")

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# libusb Windows static library
add_library(usb1 STATIC
    ../libusb/libusb/core.c
    ../libusb/libusb/descriptor.c
    ../libusb/libusb/hotplug.c
    ../libusb/libusb/io.c
    ../libusb/libusb/sync.c
    ../libusb/libusb/strerror.c
    ../libusb/libusb/os/windows_winusb.c
    ../libusb/libusb/os/windows_usbdk.c
    ../libusb/libusb/os/windows_common.c
    ../libusb/libusb/os/events_windows.c
    ../libusb/libusb/os/threads_windows.c)

target_include_directories(usb1 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../libusb/libusb)

target_include_directories(usb1 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../libusb/libusb/os
    ${CMAKE_CURRENT_SOURCE_DIR}/../libusb/msvc)

target_compile_options(usb1 PRIVATE /W0)

# audio_engine_windows static library
add_library(audio_engine_windows STATIC
    ../usb_audio.cpp)

target_include_directories(audio_engine_windows PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../libusb/libusb)

target_compile_options(audio_engine_windows PRIVATE /W3 /O2)

target_link_libraries(audio_engine_windows PRIVATE usb1)
