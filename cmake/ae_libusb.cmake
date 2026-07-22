# ae_libusb.cmake — one place that knows how to build the vendored libusb.
#
# Every platform used to copy-paste this: the common core sources are identical,
# only the os/*.c backend list and the config.h location differ. ae_add_libusb()
# selects the right set for the target platform so the Android, Linux, and
# (parked) Windows builds all share this single definition.
#
# Requires the caller to have set:
#   AE_THIRD_PARTY        - path to <repo>/third_party
# Optional:
#   AE_LIBUSB_CONFIG_DIR  - dir containing a hand-rolled config.h (desktop Linux).
#                           Android/Windows use the vendored configs instead.

function(ae_add_libusb TARGET)
    set(_lu ${AE_THIRD_PARTY}/libusb/libusb)

    set(_common
        ${_lu}/core.c
        ${_lu}/descriptor.c
        ${_lu}/hotplug.c
        ${_lu}/io.c
        ${_lu}/sync.c
        ${_lu}/strerror.c)

    if(WIN32)
        set(_os
            ${_lu}/os/windows_winusb.c
            ${_lu}/os/windows_usbdk.c
            ${_lu}/os/windows_common.c
            ${_lu}/os/events_windows.c
            ${_lu}/os/threads_windows.c)
    else()
        # Linux + Android share the usbfs backend (Android's kernel is Linux).
        set(_os
            ${_lu}/os/linux_usbfs.c
            ${_lu}/os/linux_netlink.c
            ${_lu}/os/events_posix.c
            ${_lu}/os/threads_posix.c)
    endif()

    add_library(${TARGET} STATIC ${_common} ${_os})

    # PUBLIC: consumers include "libusb.h"; PRIVATE: libusb's own internal headers.
    target_include_directories(${TARGET} PUBLIC ${_lu})
    target_include_directories(${TARGET} PRIVATE ${_lu}/os ${AE_THIRD_PARTY})

    # config.h selection per platform.
    if(ANDROID)
        target_include_directories(${TARGET} PRIVATE ${AE_THIRD_PARTY}/libusb/android)
    elseif(WIN32)
        target_include_directories(${TARGET} PRIVATE ${AE_THIRD_PARTY}/libusb/msvc)
        target_compile_options(${TARGET} PRIVATE /W0)
    else()
        if(NOT DEFINED AE_LIBUSB_CONFIG_DIR)
            message(FATAL_ERROR "ae_add_libusb: set AE_LIBUSB_CONFIG_DIR for desktop builds")
        endif()
        target_include_directories(${TARGET} PRIVATE ${AE_LIBUSB_CONFIG_DIR})
    endif()

    if(NOT WIN32)
        target_compile_options(${TARGET} PRIVATE -Wno-unused-parameter -Wno-sign-compare)
    endif()
endfunction()
