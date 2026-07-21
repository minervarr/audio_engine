// Smoke test: enumerate USB Audio Class devices via UsbAudioDriver.
// Proves core/usb_audio.cpp compiles and runs on desktop Linux.

#include <cstdio>
#include "usb_audio.h"

int main() {
    auto devices = UsbAudioDriver::enumerateUsbAudioDevices();
    if (devices.empty()) {
        printf("No USB Audio Class devices found.\n");
        return 0;
    }
    printf("%-6s %-6s %s\n", "VID", "PID", "NAME");
    for (const auto& d : devices) {
        printf("%04x   %04x   %s\n", d.vid, d.pid, d.name.c_str());
    }
    return 0;
}
