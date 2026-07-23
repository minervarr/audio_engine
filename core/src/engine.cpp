#include "core/engine.hpp"

#include <algorithm>
#include <chrono>

#include "core/dsp/eq_processor.h"
#include "core/gapless_decoder.h"

namespace ae {

namespace {
constexpr int    kReadChunk    = 16384;  // output-thread read granularity
constexpr int    kDecodeChunk  = 32768;  // decode-thread PCM scratch
constexpr int    kMinRingBytes = 64 * 1024;
constexpr int64_t kDrainCapMs  = 8000;   // safety cap above the deepest device buffer

// Map an AudioFormat to the EqProcessor encoding constant (Android AudioFormat
// values): 2=PCM_16BIT, 4=PCM_FLOAT, 21=PCM_24BIT_PACKED, 22=PCM_32BIT.
int eqEncoding(const AudioFormat& f) {
    if (f.isFloat) return 4;
    switch (f.subslotBytes) {
        case 3:  return 21;
        case 4:  return 22;
        default: return 2;   // 16-bit and anything narrower
    }
}
} // namespace

Engine::Engine(std::unique_ptr<AudioSink> sink)
    : sink_(std::move(sink)), eq_(new EqProcessor()) {}

Engine::~Engine() { stop(); }

bool Engine::play(std::unique_ptr<Decoder> decoder) {
    stop();  // tear down any prior playback

    decoder_ = std::move(decoder);
    if (!decoder_) return false;

    format_ = decoder_->format();
    {
        std::lock_guard<std::mutex> lk(sinkMtx_);
        if (!format_.valid() || !sink_->configure(format_)) {
            state_.store(State::Idle, std::memory_order_relaxed);
            if (onError_) onError_(2 /* AE_ERR_INVALID_ARG */);
            return false;
        }
        // Let a device-coupled decoder (DSD DoP: subslot depends on the DAC)
        // adapt to the negotiated wire format, then adopt its final format.
        decoder_->onOutputFormat(sink_->activeFormat());
    }
    format_ = decoder_->format();
    // (Re)configure the EQ for this track's channel count / encoding.
    {
        std::lock_guard<std::mutex> lk(eqMtx_);
        applyEqConfigLocked();
    }
    // One second of audio, floored so tiny formats still get a usable ring.
    size_t ringBytes = std::max<size_t>(
        kMinRingBytes,
        (size_t)format_.sampleRate * (size_t)format_.frameBytes());
    pcm_ = std::unique_ptr<NativePcmBuffer>(new NativePcmBuffer(ringBytes));

    framesRendered_.store(0, std::memory_order_relaxed);
    seekBaseMs_.store(0, std::memory_order_relaxed);
    seeking_.store(false, std::memory_order_relaxed);
    paused_.store(false, std::memory_order_relaxed);
    stopped_.store(false, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lk(sinkMtx_);
        if (!sink_->start()) {
            stopped_.store(true, std::memory_order_relaxed);
            state_.store(State::Idle, std::memory_order_relaxed);
            if (onError_) onError_(4 /* AE_ERR_DEVICE */);
            return false;
        }
    }

    state_.store(State::Playing, std::memory_order_relaxed);
    if (onPrepared_) onPrepared_();

    decodeThread_ = std::thread(&Engine::decodeLoop, this);
    outputThread_ = std::thread(&Engine::outputLoop, this);
    return true;
}

void Engine::decodeLoop() {
    std::unique_ptr<uint8_t[]> buf(new uint8_t[kDecodeChunk]);

    // Gapless trimmer: skips the encoder delay at the head and holds back the
    // padding at the tail, discarding it at EOS. Rebuilt on a gapless swap.
    int frameBytes = std::max(1, format_.frameBytes());
    std::unique_ptr<GaplessDecoder> trim(new GaplessDecoder(
        decoder_->encoderDelayFrames(), decoder_->encoderPaddingFrames(),
        frameBytes, pcm_.get()));
    bool wasSeeking = false;

    while (!stopped_.load(std::memory_order_relaxed)) {
        if (seeking_.load(std::memory_order_relaxed)) {
            wasSeeking = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        if (wasSeeking) { wasSeeking = false; trim->resetAfterSeek(); }

        int n;
        {
            std::lock_guard<std::mutex> lk(decodeMtx_);
            if (seeking_.load(std::memory_order_relaxed)
                || stopped_.load(std::memory_order_relaxed)) continue;
            n = decoder_->read(buf.get(), kDecodeChunk);
        }

        if (n == -1) {                 // clean end of stream
            // Gapless: if a same-format successor is queued, swap to it with no
            // gap — discard this trimmer's tail (the padding) and keep feeding.
            bool swapped = false;
            {
                std::lock_guard<std::mutex> lk(decodeMtx_);
                if (nextDecoder_ && formatsMatch(nextDecoder_->format(), format_)) {
                    decoder_->close();
                    decoder_ = std::move(nextDecoder_);
                    swapped = true;
                }
            }
            if (swapped) {
                seekBaseMs_.store(0, std::memory_order_relaxed);
                framesRendered_.store(0, std::memory_order_relaxed);
                trim.reset(new GaplessDecoder(
                    decoder_->encoderDelayFrames(), decoder_->encoderPaddingFrames(),
                    frameBytes, pcm_.get()));
                if (onTransition_) onTransition_();
                continue;
            }
            trim->signalEnd();         // discards padding tail, marks ring EOS
            break;
        }
        if (n < -1) {                  // fatal decode error
            pcm_->signalEnd();
            if (onError_) onError_(5 /* AE_ERR_DECODE */);
            break;
        }
        if (n == 0) continue;          // nothing this cycle; read() already blocked
        if (!trim->processFrame(buf.get(), 0, n)) {
            // Ring flushed: a seek (keep going) or a stop (shut down).
            if (stopped_.load(std::memory_order_relaxed)) break;
            continue;
        }
    }
}

void Engine::outputLoop() {
    std::unique_ptr<uint8_t[]> buf(new uint8_t[kReadChunk]);
    const int frameBytes = std::max(1, format_.frameBytes());

    while (!stopped_.load(std::memory_order_relaxed)) {
        // Gate on pause.
        {
            std::unique_lock<std::mutex> lk(pauseMtx_);
            pauseCv_.wait(lk, [&] {
                return !paused_.load(std::memory_order_relaxed)
                    || stopped_.load(std::memory_order_relaxed);
            });
        }
        if (stopped_.load(std::memory_order_relaxed)) break;

        int r = pcm_->read(buf.get(), 0, kReadChunk);
        if (r == -1) {                 // end of stream, buffer drained
            drainSink();
            if (!stopped_.load(std::memory_order_relaxed)) {
                state_.store(State::Stopped, std::memory_order_relaxed);
                if (onCompletion_) onCompletion_();
            }
            break;
        }
        if (r <= 0) continue;          // -2 flush, or spurious wakeup

        // Parametric EQ in-place on the decoded PCM, before it hits the device.
        // Never on DSD — a 1-bit stream can't be filtered (it's raw-passed).
        if (!format_.isDsd) {
            std::lock_guard<std::mutex> lk(eqMtx_);
            if (eqEnabled_ && eqNumFilters_ > 0) eq_->process(buf.get(), r);
        }

        int off = 0;
        while (off < r && !stopped_.load(std::memory_order_relaxed)) {
            int w;
            {
                std::lock_guard<std::mutex> lk(sinkMtx_);
                w = sink_->write(buf.get() + off, r - off);
            }
            if (w < 0) {               // device error
                if (onError_) onError_(4 /* AE_ERR_DEVICE */);
                stopped_.store(true, std::memory_order_relaxed);
                break;
            }
            if (w == 0) {              // device buffer full; brief backoff
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            off += w;
            framesRendered_.fetch_add(w / frameBytes, std::memory_order_relaxed);
        }
    }
}

// Wait for the device to render (almost) all it was handed, so a deeply-buffered
// sink (USB DAC) doesn't get its queued tail cut. AAudio reports ~0 here, so on
// the speaker this returns immediately.
void Engine::drainSink() {
    auto deadline = std::chrono::steady_clock::now()
                    + std::chrono::milliseconds(kDrainCapMs);
    while (!stopped_.load(std::memory_order_relaxed)
           && !paused_.load(std::memory_order_relaxed)) {
        int pending;
        {
            std::lock_guard<std::mutex> lk(sinkMtx_);
            pending = sink_->pendingPlaybackMs();
        }
        if (pending <= 0) break;
        if (std::chrono::steady_clock::now() >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min(pending, 30)));
    }
}

void Engine::pause() {
    if (state_.load(std::memory_order_relaxed) != State::Playing) return;
    paused_.store(true, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(sinkMtx_);
        sink_->pause();
    }
    state_.store(State::Paused, std::memory_order_relaxed);
}

void Engine::resume() {
    if (state_.load(std::memory_order_relaxed) != State::Paused) return;
    {
        std::lock_guard<std::mutex> lk(pauseMtx_);
        paused_.store(false, std::memory_order_relaxed);
    }
    pauseCv_.notify_all();
    {
        std::lock_guard<std::mutex> lk(sinkMtx_);
        sink_->resume();
    }
    state_.store(State::Playing, std::memory_order_relaxed);
}

void Engine::stop() {
    if (stopped_.exchange(true, std::memory_order_relaxed)
        && !decodeThread_.joinable() && !outputThread_.joinable()) {
        return;  // already stopped and torn down
    }
    // Unblock both threads: flush wakes reader+writer, notify wakes the pause gate.
    {
        std::lock_guard<std::mutex> lk(pauseMtx_);
        paused_.store(false, std::memory_order_relaxed);
    }
    pauseCv_.notify_all();
    if (pcm_) pcm_->flush();

    seeking_.store(false, std::memory_order_relaxed);
    joinThreads();

    {
        std::lock_guard<std::mutex> lk(sinkMtx_);
        if (sink_) sink_->stop();
    }
    {
        std::lock_guard<std::mutex> lk(decodeMtx_);
        if (decoder_)     { decoder_->close(); decoder_.reset(); }
        if (nextDecoder_) { nextDecoder_->close(); nextDecoder_.reset(); }
    }
    pcm_.reset();
    state_.store(State::Stopped, std::memory_order_relaxed);
}

void Engine::joinThreads() {
    if (decodeThread_.joinable()) decodeThread_.join();
    if (outputThread_.joinable()) outputThread_.join();
}

int64_t Engine::positionMs() const {
    if (format_.sampleRate <= 0) return 0;
    return seekBaseMs_.load(std::memory_order_relaxed)
           + framesRendered_.load(std::memory_order_relaxed) * 1000
             / format_.sampleRate;
}

int64_t Engine::durationMs() const {
    std::lock_guard<std::mutex> lk(decodeMtx_);
    return decoder_ ? decoder_->durationMs() : -1;
}

// --- seek + gapless ----------------------------------------------------------

bool Engine::formatsMatch(const AudioFormat& a, const AudioFormat& b) {
    return a.sampleRate == b.sampleRate && a.channels == b.channels
        && a.subslotBytes == b.subslotBytes && a.isFloat == b.isFloat;
}

void Engine::enqueueNext(std::unique_ptr<Decoder> decoder) {
    std::lock_guard<std::mutex> lk(decodeMtx_);
    if (nextDecoder_) nextDecoder_->close();
    nextDecoder_ = std::move(decoder);
}

bool Engine::seekMs(int64_t positionMs) {
    if (stopped_.load(std::memory_order_relaxed) || !format_.valid()) return false;
    if (positionMs < 0) positionMs = 0;

    seeking_.store(true, std::memory_order_relaxed);
    // Drop everything buffered so nothing pre-seek is rendered.
    pcm_->flush();
    {
        std::lock_guard<std::mutex> lk(eqMtx_);
        eq_->reset();
    }
    {
        std::lock_guard<std::mutex> lk(sinkMtx_);
        sink_->pause();
        sink_->flush();
        sink_->resume();
    }
    bool ok;
    {
        std::lock_guard<std::mutex> lk(decodeMtx_);
        ok = decoder_->seekMs(positionMs);
    }
    seekBaseMs_.store(positionMs, std::memory_order_relaxed);
    framesRendered_.store(0, std::memory_order_relaxed);
    seeking_.store(false, std::memory_order_relaxed);
    // Wake threads waiting on the (now-cleared) flush condition.
    pauseCv_.notify_all();
    return ok;
}

// --- output switching --------------------------------------------------------

bool Engine::switchSink(std::unique_ptr<AudioSink> newSink) {
    if (!newSink) return false;
    std::lock_guard<std::mutex> lk(sinkMtx_);

    const bool wasPlaying = state_.load(std::memory_order_relaxed) == State::Playing;

    // Retire the old sink: pause + drop its queued tail + release the device.
    if (sink_) {
        sink_->pause();
        sink_->flush();
        sink_->stop();
    }
    sink_ = std::move(newSink);

    // Restore volume state on the new device before it starts producing sound.
    sink_->setVolumeMode(volumeMode_);
    sink_->setVolume(volumeLinear_);

    // If a track is loaded, bring the new sink up on the current format so the
    // output thread's next write lands on it. With no track (switch before
    // play), just adopt the pointer — play() will configure it.
    if (format_.valid()) {
        if (!sink_->configure(format_)) return false;
        if (!sink_->start())            return false;
        if (!wasPlaying) sink_->pause();   // keep a paused engine silent
    }
    return true;
}

// --- volume (forwarded to the active sink) -----------------------------------

void Engine::setVolume(float linear01) {
    std::lock_guard<std::mutex> lk(sinkMtx_);
    volumeLinear_ = linear01;
    if (sink_) sink_->setVolume(linear01);
}

void Engine::setVolumeMode(int mode) {
    std::lock_guard<std::mutex> lk(sinkMtx_);
    volumeMode_ = mode;
    if (sink_) sink_->setVolumeMode(mode);
}

bool Engine::hasHardwareVolume() const {
    std::lock_guard<std::mutex> lk(sinkMtx_);
    return sink_ && sink_->hasHardwareVolume();
}

// --- parametric EQ -----------------------------------------------------------

void Engine::setEq(int numFilters, const double* coeffs, double preamp, bool enabled) {
    std::lock_guard<std::mutex> lk(eqMtx_);
    if (numFilters < 0) numFilters = 0;
    eqNumFilters_ = numFilters;
    eqPreamp_     = preamp;
    eqEnabled_    = enabled && numFilters > 0;
    eqCoeffs_.assign(coeffs, coeffs + (size_t)numFilters * 5);
    if (format_.valid()) applyEqConfigLocked();
}

void Engine::applyEqConfigLocked() {
    if (eqNumFilters_ > 0 && !eqCoeffs_.empty()) {
        eq_->configure(eqNumFilters_, eqCoeffs_.data(), eqPreamp_,
                       format_.channels, eqEncoding(format_));
    }
    eq_->setEnabled(eqEnabled_);
}

} // namespace ae
