#ifndef AE_CORE_ENGINE_H
#define AE_CORE_ENGINE_H

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "core/audio_format.h"
#include "core/audio_sink.h"
#include "core/decoder.hpp"
#include "core/buffer/pcm_buffer.h"

// Parametric EQ processor (pure C++, in core/dsp). Held by unique_ptr so this
// header needn't pull in its NEON intrinsics; the impl (engine.cpp) includes it.
class EqProcessor;

namespace ae {

// The platform-agnostic playback orchestrator. A decoder (encoded -> PCM) feeds
// a ring buffer that a sink (PCM -> device) drains, on two threads, exactly like
// the Java AudioEngine's decode/output threads — but pure C++ with no OS or JVM
// dependency. Platform code injects the concrete Decoder and AudioSink.
//
// Phase 2a scope: single-track play/pause/resume/stop. Phase 2b adds live
// output switching (speaker <-> USB DAC), volume (forwarded to the active sink),
// and a parametric EQ applied to the decoded PCM. Gapless, seek and DSD follow.
class Engine {
public:
    enum class State { Idle, Prepared, Playing, Paused, Stopped };

    // Takes ownership of the output sink; it lives for the engine's lifetime.
    explicit Engine(std::unique_ptr<AudioSink> sink);
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Callbacks fire from the output thread — keep them short and non-blocking.
    void setOnPrepared(std::function<void()> cb)   { onPrepared_   = std::move(cb); }
    void setOnCompletion(std::function<void()> cb) { onCompletion_ = std::move(cb); }
    void setOnError(std::function<void(int)> cb)   { onError_      = std::move(cb); }
    // Fires when a gapless successor becomes the active track (from the decode
    // thread), so the host can update "now playing".
    void setOnTransition(std::function<void()> cb) { onTransition_ = std::move(cb); }

    // Begin playing `decoder` (already open()'d). Stops any current playback
    // first. Returns false if the sink can't be configured for its format.
    bool play(std::unique_ptr<Decoder> decoder);

    // Queue a successor for gapless playback. When the current track hits EOS,
    // if `decoder`'s format matches the current one the engine swaps to it with
    // no gap (encoder padding of A and delay of B are trimmed) and fires
    // onTransition. A format mismatch is left queued and the current track
    // completes normally. Takes ownership.
    void enqueueNext(std::unique_ptr<Decoder> decoder);

    // Seek the current track to `positionMs`. Flushes the pipeline, seeks the
    // decoder, and resumes. No-op if nothing is loaded or the decoder can't seek.
    bool seekMs(int64_t positionMs);

    void pause();
    void resume();
    void stop();

    // Swap the active output sink mid-playback (e.g. speaker -> USB DAC), with
    // no gap: the shared PCM ring keeps feeding, only the drain target changes.
    // Pauses/flushes/stops the old sink, then configures + starts `newSink` for
    // the current format and restores volume state. Returns false if the new
    // sink can't take the current format. Ownership transfers in.
    bool switchSink(std::unique_ptr<AudioSink> newSink);

    // Volume is forwarded to whatever sink is active now and remembered so a
    // later switchSink() re-applies it. See AudioSink's volume contract.
    void setVolume(float linear01);
    void setVolumeMode(int mode);
    bool hasHardwareVolume() const;

    // Configure the parametric EQ applied to decoded PCM before the sink.
    // `coeffs` is numFilters * 5 doubles ([b0,b1,b2,a1,a2] per biquad, a0=1),
    // computed by the host for the current sample rate. Applied engine-wide,
    // independent of which sink is active.
    void setEq(int numFilters, const double* coeffs, double preamp, bool enabled);

    State state() const      { return state_.load(std::memory_order_relaxed); }
    int64_t positionMs() const;
    int64_t durationMs() const;

private:
    void decodeLoop();
    void outputLoop();
    void joinThreads();
    void drainSink();
    void applyEqConfigLocked();   // caller holds eqMtx_
    static bool formatsMatch(const AudioFormat& a, const AudioFormat& b);

    mutable std::mutex                sinkMtx_;   // guards sink_ + volume state
    std::unique_ptr<AudioSink>        sink_;
    mutable std::mutex                decodeMtx_; // guards decoder_ + nextDecoder_
    std::unique_ptr<Decoder>          decoder_;
    std::unique_ptr<Decoder>          nextDecoder_;   // gapless successor (queued)
    std::unique_ptr<NativePcmBuffer>  pcm_;
    AudioFormat                       format_{};

    std::thread decodeThread_;
    std::thread outputThread_;

    std::atomic<bool>    stopped_{true};
    std::atomic<bool>    paused_{false};
    std::atomic<bool>    seeking_{false};
    std::atomic<State>   state_{State::Idle};
    std::atomic<int64_t> framesRendered_{0};
    std::atomic<int64_t> seekBaseMs_{0};    // position offset after a seek / gapless swap

    std::mutex              pauseMtx_;
    std::condition_variable pauseCv_;

    // Volume state (default unity = bit-perfect); mirrored onto the active sink.
    float volumeLinear_ = 1.0f;
    int   volumeMode_   = AudioSink::VOL_AUTO;

    // EQ state. eq_ processes decoded PCM in-place in the output thread; guarded
    // by eqMtx_ so the host can retune it live. Coeffs kept so a format change
    // (new track / sample rate) re-configures with the right channels/encoding.
    std::mutex                   eqMtx_;
    std::unique_ptr<EqProcessor> eq_;
    std::vector<double>          eqCoeffs_;
    double                       eqPreamp_     = 1.0;
    int                          eqNumFilters_ = 0;
    bool                         eqEnabled_    = false;

    std::function<void()>    onPrepared_;
    std::function<void()>    onCompletion_;
    std::function<void(int)> onError_;
    std::function<void()>    onTransition_;
};

} // namespace ae

#endif // AE_CORE_ENGINE_H
