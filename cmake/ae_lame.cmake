# ae_lame.cmake — one place that knows how to build the vendored libmp3lame.
#
# LAME / libmp3lame (LGPL-2.0-or-later) is the native MP3 encoder:
# backends/mp3/mp3_encoder.cpp uses it for mic -> PCM -> .mp3 recording. LAME
# ships an autotools build, not CMake, so — like ae_libusb.cmake / ae_mpg123.cmake
# — we compile the sources ourselves against a build-generated config.h kept
# outside the pristine third_party/lame tree.
#
# The source list is exactly what upstream's own
# `./configure --disable-shared --disable-frontend --disable-decoder` compiles
# into libmp3lame.a (encoder only). config.h leaves HAVE_MPGLIB/DECODE_ON_THE_FLY
# undefined, so mpglib_interface.c builds to stubs with no dangling decode
# symbols; the mpglib/*.c decode sources are therefore NOT compiled (only their
# headers are on the include path).
#
# Requires the caller to have set:
#   AE_THIRD_PARTY  - path to <repo>/third_party

function(ae_add_lame TARGET)
    set(_l ${AE_THIRD_PARTY}/lame/libmp3lame)

    # The LAME source is download-vendored, not committed (see .gitignore).
    if(NOT EXISTS ${_l}/lame.c)
        message(FATAL_ERROR
            "LAME source not found at ${_l}.\n"
            "  Run `python3 initialize_files.py` from the repo root to fetch it.")
    endif()

    set(_srcs
        ${_l}/bitstream.c    ${_l}/encoder.c      ${_l}/fft.c
        ${_l}/gain_analysis.c ${_l}/id3tag.c      ${_l}/lame.c
        ${_l}/mpglib_interface.c ${_l}/newmdct.c   ${_l}/presets.c
        ${_l}/psymodel.c     ${_l}/quantize.c     ${_l}/quantize_pvt.c
        ${_l}/reservoir.c    ${_l}/set_get.c      ${_l}/tables.c
        ${_l}/takehiro.c     ${_l}/util.c         ${_l}/vbrquantize.c
        ${_l}/VbrTag.c       ${_l}/version.c)

    add_library(${TARGET} STATIC ${_srcs})

    if(ANDROID)
        set(_cfg ${AE_THIRD_PARTY}/lame-config/android)
    else()
        message(FATAL_ERROR "ae_add_lame: only the Android config is vendored so far")
    endif()

    # PUBLIC: consumers include "lame.h". PRIVATE: config.h + internal + mpglib
    # headers (mpglib_interface.c includes them).
    target_include_directories(${TARGET} PUBLIC  ${AE_THIRD_PARTY}/lame/include)
    target_include_directories(${TARGET} PRIVATE
        ${_cfg} ${_l} ${AE_THIRD_PARTY}/lame/mpglib)

    target_compile_definitions(${TARGET} PRIVATE HAVE_CONFIG_H)

    # Vendored C: build quietly (we do not patch upstream to fix warnings).
    target_compile_options(${TARGET} PRIVATE -w -O2)
endfunction()
