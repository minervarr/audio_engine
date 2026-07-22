// Smoke test: list physical capture ports of the running JACK server.
// Requires jackd already running (e.g. via qjackctl).

#include <cstdio>
#include "jack_source.h"

int main() {
    JackSource driver;
    if (!driver.open("audio_engine_list")) {
        fprintf(stderr, "No running JACK server (start it with qjackctl first).\n");
        return 1;
    }
    auto ports = driver.enumerateCapturePorts();
    if (ports.empty()) {
        printf("Server running, but no physical capture ports found.\n");
    } else {
        printf("Physical capture ports (server rate %d Hz):\n",
               driver.activeFormat().sampleRate);
        for (const auto& p : ports) {
            printf("  %s\n", p.portName.c_str());
        }
    }
    driver.close();
    return 0;
}
