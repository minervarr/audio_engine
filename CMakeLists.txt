cmake_minimum_required(VERSION 3.22)
project(audio_engine LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# One desktop build definition for the whole engine. Android has its own
# self-contained Gradle/CMake project under platform/android; this file (driven
# by scripts/linux/build.sh, later scripts/windows/build.ps1) builds the shared
# core, the IO backends, and the smoke-test tools.
#
#   core/       pure C++17 (header-only), zero OS deps      -> ae_core (INTERFACE)
#   backends/   OS/library-specific IO                      -> ae_usb / ae_alsa / ae_jack
#   tools/      list / capture / play smoke executables
# ---------------------------------------------------------------------------

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(AE_ROOT              ${CMAKE_CURRENT_SOURCE_DIR})
set(AE_CORE              ${AE_ROOT}/core)
set(AE_BACKENDS          ${AE_ROOT}/backends)
set(AE_THIRD_PARTY       ${AE_ROOT}/third_party)
set(AE_LIBUSB_CONFIG_DIR ${AE_ROOT}/cmake/libusb_config)

find_package(Threads REQUIRED)
include(${AE_ROOT}/cmake/ae_libusb.cmake)

# --- core: header-only interface library --------------------------------------
# Public contracts live under core/include/core/, consumed as #include "core/...".
add_library(ae_core INTERFACE)
target_include_directories(ae_core INTERFACE ${AE_CORE}/include)
target_compile_features(ae_core INTERFACE cxx_std_17)

# --- vendored libusb (single definition, see cmake/ae_libusb.cmake) -----------
ae_add_libusb(usb1)

# --- USB backend: cross-platform, playback + capture --------------------------
add_library(ae_usb STATIC ${AE_BACKENDS}/usb/usb_audio.cpp)
target_include_directories(ae_usb PUBLIC
    ${AE_BACKENDS}/usb
    ${AE_THIRD_PARTY}
    ${AE_THIRD_PARTY}/libusb/libusb)
target_link_libraries(ae_usb PUBLIC ae_core usb1 Threads::Threads)
# -O3 here is a GCC/Clang spelling. It used to be unguarded, so on Windows MSVC
# rejected it (D9002 "ignoring unknown option") and the primary USB backend
# silently built at whatever CMAKE_CXX_FLAGS_<CONFIG> gave it — the opposite of
# what this line intends. /fp:precise is MSVC's default but stated explicitly:
# the DSP downstream depends on IEEE semantics, and nothing here may be
# reassociated.
if(MSVC)
    target_compile_options(ae_usb PRIVATE /O2 /fp:precise)
else()
    target_compile_options(ae_usb PRIVATE -O3 -funroll-loops)
endif()

# --- DSP tests + benchmark ----------------------------------------------------
# Debug-only, and deliberately NOT inside the Linux block below: both are pure
# C++17 over header-only core/ code, so they build and run on every desktop
# platform. Convention matches framework/vk_canvas/core/tests/*.cc — plain
# assert(), no test framework, `#undef NDEBUG` in the source so the asserts
# survive optimization.
#
#   dsp_null_test — the bit-exactness gate. Holds a frozen copy of the DSP as
#                   it stood before any optimization and asserts the live code
#                   still produces identical output. Every optimization phase
#                   must leave this passing.
#   dsp_bench     — ns/sample for the same paths, so a change can be shown to
#                   be worth making. Compiled -O3 even in a Debug build:
#                   benchmarking unoptimized code measures nothing.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    # backends/usb is on the include path for usb_pack.h only — a header-only,
    # libusb-free piece of the USB backend, which is why it can be tested
    # without linking ae_usb or opening a device.
    add_executable(dsp_null_test ${AE_ROOT}/tests/dsp_null_test.cpp)
    target_link_libraries(dsp_null_test PRIVATE ae_core)
    target_include_directories(dsp_null_test PRIVATE ${AE_BACKENDS}/usb)

    add_executable(dsp_bench ${AE_ROOT}/tests/dsp_bench.cpp)
    target_link_libraries(dsp_bench PRIVATE ae_core)
    target_include_directories(dsp_bench PRIVATE ${AE_BACKENDS}/usb)

    foreach(t dsp_null_test dsp_bench)
        if(MSVC)
            target_compile_options(${t} PRIVATE /O2 /fp:precise)
        else()
            target_compile_options(${t} PRIVATE -O3 -Wall)
        endif()
    endforeach()
endif()

# --- Linux-only backends + tools ----------------------------------------------
if(UNIX AND NOT APPLE AND NOT ANDROID)
    # ALSA: direct-hardware capture + playback, no sound server.
    find_package(ALSA REQUIRED)
    add_library(ae_alsa STATIC
        ${AE_BACKENDS}/alsa/alsa_source.cpp
        ${AE_BACKENDS}/alsa/alsa_sink.cpp)
    target_include_directories(ae_alsa PUBLIC ${AE_BACKENDS}/alsa)
    target_link_libraries(ae_alsa PUBLIC ae_core ALSA::ALSA Threads::Threads)

    # JACK2: client of the user's running jackd. Real libjack, NEVER pipewire-jack.
    find_package(PkgConfig)
    if(PkgConfig_FOUND)
        pkg_check_modules(JACK jack)
    endif()
    if(JACK_FOUND)
        add_library(ae_jack STATIC
            ${AE_BACKENDS}/jack/jack_source.cpp
            ${AE_BACKENDS}/jack/jack_sink.cpp)
        target_include_directories(ae_jack PUBLIC ${AE_BACKENDS}/jack ${JACK_INCLUDE_DIRS})
        target_link_libraries(ae_jack PUBLIC ae_core ${JACK_LIBRARIES} Threads::Threads)
    else()
        message(WARNING "libjack not found via pkg-config — skipping ae_jack. "
                        "Install jack2 (NOT pipewire-jack) and reconfigure.")
    endif()

    # --- smoke-test tools -----------------------------------------------------
    option(AUDIO_ENGINE_BUILD_TOOLS "Build capture/play smoke-test executables" ON)
    if(AUDIO_ENGINE_BUILD_TOOLS)
        add_executable(list_usb_devices    tools/list_usb_devices.cpp)
        target_link_libraries(list_usb_devices ae_usb)
        add_executable(capture_usb_to_wav  tools/capture_usb_to_wav.cpp)
        target_link_libraries(capture_usb_to_wav ae_usb)

        add_executable(list_alsa_devices   tools/list_alsa_devices.cpp)
        target_link_libraries(list_alsa_devices ae_alsa)
        add_executable(capture_alsa_to_wav tools/capture_alsa_to_wav.cpp)
        target_link_libraries(capture_alsa_to_wav ae_alsa)
        add_executable(play_wav_to_alsa    tools/play_wav_to_alsa.cpp)
        target_link_libraries(play_wav_to_alsa ae_alsa)

        if(JACK_FOUND)
            add_executable(list_jack_ports     tools/list_jack_ports.cpp)
            target_link_libraries(list_jack_ports ae_jack)
            add_executable(capture_jack_to_wav tools/capture_jack_to_wav.cpp)
            target_link_libraries(capture_jack_to_wav ae_jack)
            add_executable(play_wav_to_jack    tools/play_wav_to_jack.cpp)
            target_link_libraries(play_wav_to_jack ae_jack)
        endif()
    endif()
endif()
