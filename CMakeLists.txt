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

# --- 32-bit Android (armeabi-v7a): two bionic quirks in the vendored decoders ---
# Both matter only on 32-bit ABIs and are no-ops on arm64-v8a/x86_64, so the
# definitions are applied unconditionally under Android to keep every TU in the
# .so consistent.
# 1. libFLAC: its HAVE_FSEEKO probe cannot win on 32-bit bionic (fseeko is
#    __RENAME'd to fseeko64 at the ELF level, so the link probe finds no
#    `fseeko` symbol), and its pre-API-24 guard can misfire the same way.
#    compat.h then does `#define fseeko fseek`, which collides with bionic's
#    REAL fseeko declaration (API >= 24) and breaks the build. bionic has had
#    real fseeko/ftello since API 24 and this engine's floor is 26, so defining
#    HAVE_FSEEKO at compile level is the truthful answer — and compat.h only
#    ever tests it with #ifndef, so an empty-body definition is enough (it also
#    matches the `#cmakedefine` config.h emits on the ABIs where the probe
#    succeeds, avoiding a redefinition warning).
# 2. libmpg123: the committed third_party/mpg123-config/android/config.h
#    hardcodes SIZEOF_OFF_T 8; on 32-bit off_t is 4 bytes unless LFS is on, and
#    lfs_wrap.c's static assert fires. _FILE_OFFSET_BITS=64 makes off_t 64-bit
#    (exactly what that config was generated against). Harmless on 64-bit.
if(ANDROID)
    add_compile_definitions(_FILE_OFFSET_BITS=64 HAVE_FSEEKO)
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
# usb_audio.cpp logs through __android_log_print on Android (see its LOG* macros),
# so the target that compiles it has to bring liblog with it. Without this, every
# consumer outside platform/android/'s monolithic build hits undefined references
# to __android_log_print and has to guess that `log` is the missing piece --
# Matrix_Player got it from the monolith, AOAS links this target directly.
if(ANDROID)
    target_link_libraries(ae_usb PUBLIC log)
endif()
# -O3 here is a GCC/Clang spelling. It used to be unguarded, so on Windows MSVC
# rejected it (D9002 "ignoring unknown option") and the primary USB backend
# silently built at whatever CMAKE_CXX_FLAGS_<CONFIG> gave it — the opposite of
# what this line intends. /fp:precise is MSVC's default but stated explicitly:
# the DSP downstream depends on IEEE semantics, and nothing here may be
# reassociated.
if(MSVC)
    target_compile_options(ae_usb PRIVATE /O2 /fp:precise)
else()
    target_compile_options(ae_usb PRIVATE -O3 -funroll-loops -Wall -Wextra -Wconversion)
endif()

# --- Decoder backends ---------------------------------------------------------
# FLAC / MP3 / DSD decode, the same first-party implementations Android has
# used since Phase 2.5/2.6 — built here so desktop stops needing a separate
# single-header decoder in the consuming app. All three implement ae::Decoder
# (core/include/core/decoder.hpp) and are selected by magic bytes, not by
# extension. Cross-platform C++17 over fd-based IO.
#
# Opt-out exists because two of them carry vendored dependencies: libFLAC is a
# submodule (may be unpopulated in a shallow clone) and libmpg123 is a
# download (initialize_files.py). A consumer that wants neither can build the
# USB path alone, as before.
option(AUDIO_ENGINE_BUILD_DECODERS "Build the FLAC/MP3/DSD decoder backends" ON)

if(AUDIO_ENGINE_BUILD_DECODERS)
    # DSD: pure C++17, zero external deps — DFF/DSF parsing plus the DoP/native
    # packer in core/dsp/dsd/. Nothing to vendor, so it is unconditional.
    add_library(ae_dsd STATIC
        ${AE_BACKENDS}/dsd/dsd_decoder.cpp
        ${AE_BACKENDS}/dsd/dsf_parser.cpp
        ${AE_BACKENDS}/dsd/dff_parser.cpp)
    target_include_directories(ae_dsd PUBLIC ${AE_BACKENDS}/dsd)
    target_link_libraries(ae_dsd PUBLIC ae_core)

    # FLAC: vendored libFLAC submodule, kept pristine — configured ONLY through
    # cache vars (never patched), same rule the Android build follows. Its
    # default ON options would otherwise build the flac/metaflac CLI tools,
    # libFLAC++, the test suite and docs, none of which we link.
    if(EXISTS ${AE_THIRD_PARTY}/flac/CMakeLists.txt)
        set(BUILD_CXXLIBS              OFF CACHE BOOL "" FORCE)
        set(BUILD_PROGRAMS             OFF CACHE BOOL "" FORCE)
        set(BUILD_EXAMPLES             OFF CACHE BOOL "" FORCE)
        set(BUILD_TESTING              OFF CACHE BOOL "" FORCE)
        set(BUILD_DOCS                 OFF CACHE BOOL "" FORCE)
        set(INSTALL_MANPAGES           OFF CACHE BOOL "" FORCE)
        set(INSTALL_PKGCONFIG_MODULES  OFF CACHE BOOL "" FORCE)
        set(INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
        set(WITH_OGG                   OFF CACHE BOOL "" FORCE)
        add_subdirectory(${AE_THIRD_PARTY}/flac ${CMAKE_BINARY_DIR}/third_party/flac
                         EXCLUDE_FROM_ALL)

        add_library(ae_flac STATIC ${AE_BACKENDS}/flac/flac_decoder.cpp)
        target_include_directories(ae_flac PUBLIC ${AE_BACKENDS}/flac)
        target_link_libraries(ae_flac PUBLIC ae_core FLAC)
    else()
        message(WARNING "third_party/flac is empty — skipping ae_flac. "
                        "Run `git submodule update --init --recursive`.")
    endif()

    # MP3: vendored libmpg123 (downloaded, not committed). ae_add_mpg123 itself
    # hard-errors with the fetch instruction when the source is missing, so
    # check first and degrade to a warning — an absent optional codec must not
    # break a build that only wants USB + FLAC.
    if(EXISTS ${AE_THIRD_PARTY}/mpg123/src/libmpg123/libmpg123.c)
        include(${AE_ROOT}/cmake/ae_mpg123.cmake)
        ae_add_mpg123(mpg123)
        add_library(ae_mp3 STATIC ${AE_BACKENDS}/mp3/mp3_decoder.cpp)
        target_include_directories(ae_mp3 PUBLIC ${AE_BACKENDS}/mp3)
        target_link_libraries(ae_mp3 PUBLIC ae_core mpg123)
    else()
        message(WARNING "third_party/mpg123 not fetched — skipping ae_mp3. "
                        "Run `python3 initialize_files.py`.")
    endif()
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

# --- Android-only backends ----------------------------------------------------
#
# Same shape as the ALSA/JACK block below: a real library target a consuming app
# can link, with its include directory PUBLIC so the app does not restate paths.
#
# This did not exist. backends/aaudio/ has been committed since c222e34, but the
# only CMake that ever compiled it was platform/android/'s own self-contained
# Gradle project, which builds ONE library for this engine's own app and exports
# nothing. A consumer (Matrix_Player/android) therefore linked `ae_aaudio` and
# got "aaudio_sink.h file not found" -- a target that was never defined.
#
# Directive 4 says platforms are equal peers, each with its own build entry;
# that is about entry POINTS, not about which platforms get reusable targets.
# Linux has ae_alsa/ae_jack, so Android gets ae_aaudio.
if(ANDROID)
    # SINK ONLY, deliberately. aaudio_source.cpp is capture, which no consumer
    # asks for today, and backends/mediacodec/ (AMediaCodec hardware decode) is
    # not built at all: this engine decodes FLAC and MP3 with its own vendored
    # libFLAC/libmpg123 ON PURPOSE, because some phones ship no FLAC decoder and
    # a per-device decode path would make "bit-perfect everywhere" a claim that
    # depends on the handset. Adding either later is one line each.
    add_library(ae_aaudio STATIC ${AE_BACKENDS}/aaudio/aaudio_sink.cpp)
    target_include_directories(ae_aaudio PUBLIC ${AE_BACKENDS}/aaudio)
    # `aaudio` is the NDK's own libaaudio (API 26+, which is this engine's floor
    # on Android anyway). It is the ONLY place <aaudio/AAudio.h> is included --
    # directive 2, the single-includer rule.
    target_link_libraries(ae_aaudio PUBLIC ae_core aaudio)
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
