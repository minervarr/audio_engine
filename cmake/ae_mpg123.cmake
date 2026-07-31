# ae_mpg123.cmake — one place that knows how to build the vendored libmpg123.
#
# libmpg123 (LGPL-2.1) is the native MP3 decoder: backends/mp3/mp3_decoder.cpp
# uses it so MP3 decodes identically on every device, instead of the per-OEM
# AMediaCodec lottery. mpg123 ships an autotools build, not CMake, so — like
# ae_libusb.cmake — we compile the sources ourselves against a build-generated
# config.h kept outside the pristine third_party/mpg123 tree.
#
# The source list and defines are exactly what upstream's own
# `./configure --with-cpu=generic --disable-modules` compiles into libmpg123.a
# (verified against the generated Makefile): the generic decoder, no assembly,
# no runtime CPU dispatch. NEON/SSE synth variants could be added later.
#
# Requires the caller to have set:
#   AE_THIRD_PARTY  - path to <repo>/third_party

function(ae_add_mpg123 TARGET)
    set(_m   ${AE_THIRD_PARTY}/mpg123/src)
    set(_lib ${_m}/libmpg123)

    # The mpg123 source is download-vendored, not committed (see .gitignore).
    if(NOT EXISTS ${_lib}/libmpg123.c)
        message(FATAL_ERROR
            "mpg123 source not found at ${_lib}.\n"
            "  Run `python3 initialize_files.py` from the repo root to fetch it.")
    endif()

    # libmpg123 core (generic build) + the compat shim it links.
    set(_srcs
        ${_lib}/parse.c      ${_lib}/frame.c     ${_lib}/format.c
        ${_lib}/dct64.c      ${_lib}/id3.c       ${_lib}/optimize.c
        ${_lib}/readers.c    ${_lib}/tabinit.c   ${_lib}/libmpg123.c
        ${_lib}/index.c      ${_lib}/layer1.c    ${_lib}/layer2.c
        ${_lib}/layer3.c     ${_lib}/equalizer.c ${_lib}/synth.c
        ${_lib}/synth_8bit.c ${_lib}/synth_s32.c ${_lib}/synth_real.c
        ${_lib}/ntom.c       ${_lib}/feature.c   ${_lib}/lfs_wrap.c
        ${_lib}/icy.c        ${_lib}/icy2utf8.c  ${_lib}/stringbuf.c
        ${_m}/compat/compat.c ${_m}/compat/compat_str.c)

    add_library(${TARGET} STATIC ${_srcs})

    # config.h location: build-generated, per platform, outside the pristine tree.
    if(ANDROID)
        set(_cfg ${AE_THIRD_PARTY}/mpg123-config/android)
    elseif(UNIX AND NOT APPLE)
        # See that file's header for why it is a verbatim copy of the Android
        # one rather than its own ./configure run.
        set(_cfg ${AE_THIRD_PARTY}/mpg123-config/linux)
    else()
        message(FATAL_ERROR
            "ae_add_mpg123: no vendored config.h for this platform yet "
            "(have: android, linux). Generate one with mpg123's "
            "`./configure --with-cpu=generic --disable-modules` and drop its "
            "src/config.h into third_party/mpg123-config/<platform>/.")
    endif()

    # PUBLIC: consumers include <mpg123.h>. PRIVATE: config.h + internal headers.
    # The sources reach ../common, ../compat and ../version.h relative to their
    # own location, so the preserved src/ layout resolves those automatically.
    target_include_directories(${TARGET} PUBLIC  ${_m}/include)
    target_include_directories(${TARGET} PRIVATE ${_cfg} ${_m} ${_lib})

    # -DOPT_GENERIC -DREAL_IS_FLOAT come from upstream's CPPFLAGS, not config.h.
    target_compile_definitions(${TARGET} PRIVATE
        HAVE_CONFIG_H OPT_GENERIC REAL_IS_FLOAT)

    # Vendored C: build quietly (we do not patch upstream to fix warnings).
    # -ffast-math is upstream mpg123's own CFLAGS choice, kept as-is. It does
    # NOT contradict the engine's "never -ffast-math" rule: that rule governs
    # core/dsp/, whose output dsp_null_test asserts bit-identical across
    # platforms. This is a vendored lossy decoder built with its author's
    # flags, and nothing in core/ is compiled with them.
    if(MSVC)
        target_compile_options(${TARGET} PRIVATE /w /O2 /fp:fast)
    else()
        target_compile_options(${TARGET} PRIVATE -w -O2 -ffast-math)
    endif()
endfunction()
