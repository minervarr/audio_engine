/* Minimal hand-rolled libusb config for desktop Linux (usbfs backend).
 * Modeled on third_party/libusb/android/config.h — same kernel, same four
 * os/ source files — minus USE_SYSTEM_LOGGING_FACILITY so libusb messages
 * go to stderr instead of syslog. */
#pragma once

#define DEFAULT_VISIBILITY __attribute__ ((visibility ("default")))
#define ENABLE_LOGGING 1
#define HAVE_ASM_TYPES_H 1
#define HAVE_CLOCK_GETTIME 1
#define HAVE_NFDS_T 1
#define HAVE_PIPE2 1
#define HAVE_SYS_TIME_H 1
#define PLATFORM_POSIX 1

/* Enable attribute-based checking of printf-like arguments in logging
   functions. */
#define PRINTF_FORMAT(a, b) __attribute__ ((__format__ (__printf__, a, b)))

#define _GNU_SOURCE 1
