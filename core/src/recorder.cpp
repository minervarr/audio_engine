#include "core/recorder.hpp"

#include <memory>

namespace ae {

namespace {
constexpr int kCaptureChunk = 8192;   // bytes pulled per read
} // namespace

Recorder::Recorder(std::unique_ptr<AudioSource> source, std::unique_ptr<Encoder> encoder)
    : source_(std::move(source)), encoder_(std::move(encoder)) {}

Recorder::~Recorder() { stop(); }

bool Recorder::start(int fd, const AudioFormat& fmt) {
    if (running_.load(std::memory_order_relaxed)) return false;
    if (!source_ || !encoder_) return false;

    if (!source_->configure(fmt)) return false;
    // Encode in the format the device actually granted (rate/channels may differ).
    AudioFormat granted = source_->activeFormat();
    if (!encoder_->open(fd, granted)) return false;
    if (!source_->start()) { encoder_->close(); return false; }

    running_.store(true, std::memory_order_relaxed);
    thread_ = std::thread(&Recorder::captureLoop, this);
    return true;
}

void Recorder::captureLoop() {
    std::unique_ptr<uint8_t[]> buf(new uint8_t[kCaptureChunk]);
    while (running_.load(std::memory_order_relaxed)) {
        int n = source_->read(buf.get(), kCaptureChunk);
        if (n > 0) {
            if (!encoder_->encode(buf.get(), n)) break;   // hard encode error
        } else if (n < 0) {
            break;                                         // source failure
        }
        // n == 0: nothing buffered yet; read() already blocked, loop again.
    }
}

void Recorder::stop() {
    if (!running_.exchange(false, std::memory_order_relaxed)
        && !thread_.joinable()) {
        return;   // never started / already stopped
    }
    if (source_) source_->stop();          // unblock a pending read()
    if (thread_.joinable()) thread_.join();
    if (encoder_) encoder_->close();       // flush + patch headers
}

} // namespace ae
