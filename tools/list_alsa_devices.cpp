// Smoke test: list ALSA capture-capable devices (direct hw: access).

#include <cstdio>
#include "alsa_source.h"

int main() {
    auto devices = AlsaSource::enumerateCaptureDevices();
    if (devices.empty()) {
        printf("No ALSA capture devices found.\n");
        return 0;
    }
    printf("%-12s %s\n", "DEVICE", "NAME");
    for (const auto& d : devices) {
        printf("%-12s %s\n", d.deviceId.c_str(), d.name.c_str());
    }
    return 0;
}
