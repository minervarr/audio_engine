#ifndef AE_CORE_RECORDER_H
#define AE_CORE_RECORDER_H

#include <atomic>
#include <memory>
#include <thread>

#include "core/audio_format.h"
#include "core/audio_source.h"
#include "core/encoder.hpp"

namespace ae {

// Capture orchestrator, the mirror of Engine: pulls PCM from an AudioSource on a
// dedicated thread and feeds it to an Encoder that writes an encoded file. Pure
// C++ with no OS/JVM dependency — platform code injects the concrete source
// (AAudioSource) and encoder (FlacEncoder). Lifecycle: start() -> stop().
class Recorder {
public:
    Recorder(std::unique_ptr<AudioSource> source, std::unique_ptr<Encoder> encoder);
    ~Recorder();

    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    // Configure + start the source, open the encoder on `fd` for the granted
    // capture format, and spin the capture thread. Returns false on any failure.
    bool start(int fd, const AudioFormat& fmt);

    // Stop the capture thread and finalize the encoded file. Idempotent.
    void stop();

    bool isRecording() const { return running_.load(std::memory_order_relaxed); }

private:
    void captureLoop();

    std::unique_ptr<AudioSource> source_;
    std::unique_ptr<Encoder>     encoder_;
    std::thread                  thread_;
    std::atomic<bool>            running_{false};
};

} // namespace ae

#endif // AE_CORE_RECORDER_H
