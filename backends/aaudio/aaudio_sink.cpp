#include "aaudio_sink.h"

#include <aaudio/AAudio.h>
#include <android/log.h>
#include <time.h>

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "AAudioSink", __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "AAudioSink", __VA_ARGS__)

namespace ae {
namespace {

// AAudio's error callback, on a thread of its own.
//
// userData is the sink's `disconnected_` flag rather than the sink itself, so
// this needs no access to the class and cannot be tempted into touching
// anything else. Setting a flag is ALL that is permitted here: AAudio
// explicitly forbids closing or stopping the stream from inside this callback,
// and the documented pattern is to hand the work to another thread.
void onStreamError(AAudioStream* /*stream*/, void* userData, aaudio_result_t error) {
    LOGE("stream error: %s", AAudio_convertResultToText(error));
    if (userData) static_cast<std::atomic<bool>*>(userData)->store(true);
}

}  // namespace

AAudioSink::~AAudioSink() { closeStream(); }

bool AAudioSink::configure(const AudioFormat& fmt) {
    closeStream();
    // Kept so the stream can be rebuilt exactly as it was after the OS takes
    // it away -- see reopenAfterDisconnect().
    requested_  = fmt;
    srcSubslot_ = fmt.subslotBytes > 0 ? fmt.subslotBytes : 2;

    AAudioStreamBuilder* b = nullptr;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK || !b) {
        LOGE("createStreamBuilder failed");
        return false;
    }
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSampleRate(b, fmt.sampleRate);
    AAudioStreamBuilder_setChannelCount(b, fmt.channels);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_NONE);
    // Without this a disconnect is silent: writes start failing and the
    // position counters freeze, with nothing anywhere saying why. See
    // disconnected().
    disconnected_.store(false);
    AAudioStreamBuilder_setErrorCallback(b, onStreamError, &disconnected_);

    aaudio_result_t r = AAudioStreamBuilder_openStream(b, &stream_);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK || !stream_) {
        LOGE("openStream failed: %s", AAudio_convertResultToText(r));
        stream_ = nullptr;
        return false;
    }

    // Adopt the stream's actual granted parameters.
    format_.sampleRate   = AAudioStream_getSampleRate(stream_);
    format_.channels     = AAudioStream_getChannelCount(stream_);
    format_.bitDepth     = 16;
    format_.subslotBytes = 2;
    format_.isFloat      = false;
    LOGI("configured: %d Hz, %d ch", format_.sampleRate, format_.channels);
    return format_.valid();
}

bool AAudioSink::start() {
    return stream_ && AAudioStream_requestStart(stream_) == AAUDIO_OK;
}

// AAudio hands the stream back when the ROUTE changes underneath it.
//
// AAUDIO_ERROR_DISCONNECTED is not a failure of the write and not a device
// fault: it is AAudio saying "the thing you were playing to is not the thing
// that is there now -- open a new stream". Headphones going in or coming out
// does it, and so does every A2DP codec change, because setting a Bluetooth
// codec tears the link down and rebuilds it.
//
// Until this existed the sink returned -1 and stopped, and nothing above it
// knew to reopen anything: the music simply ended, mid-track, with the
// transport still showing PLAYING and the clock frozen. Reproduced on a moto
// g06 with LG-PL7 headphones -- "write failed: AAUDIO_ERROR_DISCONNECTED",
// once, and then silence.
//
// Rebuilt with the SAME format that was asked for originally, never the one
// the old stream happened to be granted. If the new route cannot give that
// back, the honest move is to fail: the caller's whole pipeline -- resampler,
// EQ, quantiser -- was built around that format, and quietly playing at
// another rate would be a different, wrong-pitched kind of working.
bool AAudioSink::reopenAfterDisconnect() {
    const AudioFormat prev = format_;
    LOGI("route changed: reopening the stream");

    closeStream();
    if (!configure(requested_)) {
        LOGE("could not reopen after a route change");
        return false;
    }
    if (format_.sampleRate != prev.sampleRate || format_.channels != prev.channels) {
        LOGE("the new route grants %d Hz / %d ch, not %d Hz / %d ch — refusing",
             format_.sampleRate, format_.channels, prev.sampleRate, prev.channels);
        return false;
    }
    if (!start()) {
        LOGE("reopened stream would not start");
        return false;
    }
    return true;
}

int AAudioSink::write(const uint8_t* data, int len) {
    if (!stream_) return -1;
    const int channels = format_.channels;
    if (channels <= 0) return -1;

    if (srcSubslot_ == 3) {
        // 24-bit-packed LE source -> float -> TPDF-dithered 16-bit -> speaker.
        const int srcFrameBytes = 3 * channels;
        int frames = len / srcFrameBytes;
        if (frames <= 0) return 0;
        int samples = frames * channels;
        if ((int)f32_.size() < samples) { f32_.resize(samples); i16_.resize(samples); }
        for (int i = 0; i < samples; i++) {
            const uint8_t* p = data + (size_t)i * 3;
            int32_t v = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16));
            if (v & 0x800000) v |= (int32_t)0xFF000000;   // sign-extend 24 -> 32
            f32_[i] = (float)v * (1.0f / 8388608.0f);
        }
        floatToInt16Dither(f32_.data(), i16_.data(), samples, dither_);
        aaudio_result_t r = AAudioStream_write(stream_, i16_.data(), frames, 100LL * 1000 * 1000);
        if (r == AAUDIO_ERROR_DISCONNECTED) {
            if (!reopenAfterDisconnect()) return -1;
            r = AAudioStream_write(stream_, i16_.data(), frames, 100LL * 1000 * 1000);
        }
        if (r < 0) {
            LOGE("write failed: %s", AAudio_convertResultToText(r));
            // The callback is the reliable signal and this is the immediate
            // one; whichever arrives first, the consumer sees the same flag.
            if (r == AAUDIO_ERROR_DISCONNECTED) disconnected_.store(true);
            return -1;
        }
        reportStall(r);
        return (int)r * srcFrameBytes;   // report source-domain bytes consumed
    }

    // 16-bit passthrough. Blocking write with a 100 ms cap so stop() stays responsive.
    const int frameBytes = 2 * channels;
    int32_t numFrames = len / frameBytes;
    if (numFrames <= 0) return 0;
    aaudio_result_t r = AAudioStream_write(stream_, data, numFrames, 100LL * 1000 * 1000);
    if (r == AAUDIO_ERROR_DISCONNECTED) {
        if (!reopenAfterDisconnect()) return -1;
        r = AAudioStream_write(stream_, data, numFrames, 100LL * 1000 * 1000);
    }
    if (r < 0) {
        LOGE("write failed: %s", AAudio_convertResultToText(r));
        if (r == AAUDIO_ERROR_DISCONNECTED) disconnected_.store(true);
        return -1;
    }
    reportStall(r);
    return (int)r * frameBytes;
}

// A write that takes NOTHING is not an error and AAudio reports it as success,
// so it leaves no trace anywhere — the stream simply stops consuming and the
// player's clock stands still while the state says PLAYING. That happened on a
// real handset over A2DP and cost an hour to find from outside the process,
// which is the reason this exists: one line a second, only while starved,
// naming the state the stream is actually in.
void AAudioSink::reportStall(int framesAccepted) {
    if (!stream_) return;
    if (framesAccepted > 0) { starvedWrites_ = 0; return; }

    // 100 ms per timed-out write, so ten of them is about a second.
    if (++starvedWrites_ % 10 != 0) return;
    const aaudio_stream_state_t st = AAudioStream_getState(stream_);
    LOGE("stream accepted no frames for ~%d ms — state=%s, xruns=%d, written=%lld, read=%lld",
         starvedWrites_ * 100, AAudio_convertStreamStateToText(st),
         AAudioStream_getXRunCount(stream_),
         (long long)AAudioStream_getFramesWritten(stream_),
         (long long)AAudioStream_getFramesRead(stream_));
}

void AAudioSink::pause() {
    if (stream_) AAudioStream_requestPause(stream_);
}

void AAudioSink::resume() {
    if (stream_) AAudioStream_requestStart(stream_);
}

// How long to wait for one asynchronous AAudio transition. Both callers are on
// a thread the listener is waiting on — Stop and Next are supposed to be
// instant — so this is a bound on being wrong, not a budget: the transitions
// take a fraction of a millisecond in practice.
static constexpr int64_t kTransitionTimeoutNs = 100LL * 1000 * 1000;   // 100 ms

bool AAudioSink::pauseAndDiscard() {
    if (!stream_) return false;

    // requestPause() is only legal on a STARTED output stream. Asking from any
    // other state returns an error, so read the state rather than guessing —
    // stop() can legitimately arrive on a stream that was never started.
    const aaudio_stream_state_t state = AAudioStream_getState(stream_);
    if (state != AAUDIO_STREAM_STATE_STARTING && state != AAUDIO_STREAM_STATE_STARTED) {
        // Nothing is playing, so there is nothing buffered to throw away.
        return true;
    }

    aaudio_stream_state_t next = AAUDIO_STREAM_STATE_UNINITIALIZED;

    aaudio_result_t r = AAudioStream_requestPause(stream_);
    if (r != AAUDIO_OK) {
        LOGE("requestPause failed: %s", AAudio_convertResultToText(r));
        return false;
    }
    // Pausing is asynchronous, and a flush issued while still PAUSING is
    // refused with INVALID_STATE — which is precisely the failure this whole
    // function exists to stop repeating.
    r = AAudioStream_waitForStateChange(stream_, AAUDIO_STREAM_STATE_PAUSING,
                                        &next, kTransitionTimeoutNs);
    if (r != AAUDIO_OK) {
        LOGE("waitForStateChange(PAUSING) failed: %s", AAudio_convertResultToText(r));
        return false;
    }

    r = AAudioStream_requestFlush(stream_);
    if (r != AAUDIO_OK) {
        LOGE("requestFlush failed: %s", AAudio_convertResultToText(r));
        return false;
    }
    r = AAudioStream_waitForStateChange(stream_, AAUDIO_STREAM_STATE_FLUSHING,
                                        &next, kTransitionTimeoutNs);
    if (r != AAUDIO_OK) {
        LOGE("waitForStateChange(FLUSHING) failed: %s", AAudio_convertResultToText(r));
        return false;
    }
    return true;
}

// AudioSink's contract for this is "discard any audio already handed over but
// not yet rendered" — the same thing AlsaSink does with snd_pcm_drop. This used
// to call requestFlush() on a STARTED stream, which AAudio answers with
// INVALID_STATE; the result was dropped on the floor, so nothing was ever
// discarded and every flush on the phone's speaker was silently a no-op. That
// is what made Next and seek take the rest of the buffer to be heard.
//
// Re-armed at the end, exactly as AlsaSink::flush() follows its drop with
// snd_pcm_prepare: the caller's next write must reach a stream that can accept
// it, without the caller knowing which backend it is talking to.
void AAudioSink::flush() {
    if (!stream_) return;

    // The result is deliberately ignored: a discard that got halfway leaves the
    // stream PAUSED, and returning here would strand it there — the caller's
    // next write would stall against a stream that can never accept it. Re-arm
    // on the way out whatever happened, so a failed flush costs a stale tail
    // and not the rest of the session.
    pauseAndDiscard();

    // dither_ is deliberately NOT reset. It is a bare LCG with no error
    // feedback, so where it happens to sit in its sequence has no audible
    // consequence; reseeding it would only make the noise repeat identically
    // at every track boundary.
    const aaudio_stream_state_t state = AAudioStream_getState(stream_);
    if (state == AAUDIO_STREAM_STATE_STARTING || state == AAUDIO_STREAM_STATE_STARTED)
        return;   // never paused in the first place; already able to take writes

    const aaudio_result_t r = AAudioStream_requestStart(stream_);
    if (r != AAUDIO_OK)
        LOGE("restart after flush failed: %s", AAudio_convertResultToText(r));
}

// requestStop() on its own is a DRAIN — AAudio plays out whatever is already in
// the buffer and stops after it. That is the wrong half of the contract here
// (AlsaSink::stop() drops), and it is why pressing Stop kept playing. Discard
// first, then stop what is by now an empty stream.
void AAudioSink::stop() {
    if (!stream_) return;
    pauseAndDiscard();          // best effort: stop regardless of the outcome
    const aaudio_result_t r = AAudioStream_requestStop(stream_);
    if (r != AAUDIO_OK)
        LOGE("requestStop failed: %s", AAudio_convertResultToText(r));
}

// How much audio the device still owes the listener.
//
// framesWritten - framesRead is the honest formula and it is what this used to
// return unbounded. That is wrong for one specific reason and it is not a
// rounding error: over A2DP the two counters do not track each other. The
// Bluetooth HAL takes frames in bursts and reports them read in bursts, so the
// difference climbs far past anything the stream can physically be holding.
//
// The caller divides by this. PlayerWindow computes the playback position as
// "frames written minus what is still pending", clamped so it can never go
// backwards past the start of the track — so a pending figure that grows
// without bound pins the position at 0:00 forever, and the gapless coordinator,
// which waits for pending to fall to ~0 before handing over, waits for
// something that never happens. On a moto g06 over A2DP that is exactly what
// was seen: a clock frozen at 0:00 and a track that never advanced to the next.
//
// The bound is not arbitrary. A stream cannot hold more than its own buffer, so
// anything above the capacity is a counter disagreeing with itself rather than
// audio in flight, and the capacity is the largest TRUE answer there is.
int AAudioSink::pendingPlaybackMs() const {
    if (!stream_ || format_.sampleRate <= 0) return 0;
    const int64_t written = AAudioStream_getFramesWritten(stream_);
    const int64_t read    = AAudioStream_getFramesRead(stream_);
    int64_t pending = written - read;
    if (pending < 0) pending = 0;

    const int64_t capacity = AAudioStream_getBufferCapacityInFrames(stream_);
    if (capacity > 0 && pending > capacity) {
        // Rate-limited, because when this fires it fires on every 250 ms tick.
        if (++overPending_ % 40 == 1)
            LOGE("pending %lld frames exceeds the %lld-frame buffer "
                 "(written=%lld read=%lld) — clamping",
                 (long long)pending, (long long)capacity,
                 (long long)written, (long long)read);
        pending = capacity;
    }
    return (int)(pending * 1000 / format_.sampleRate);
}

int64_t AAudioSink::framesPlayed() const {
    if (!stream_) return 0;
    return AAudioStream_getFramesRead(stream_);
}

int64_t AAudioSink::framesWritten() const {
    if (!stream_) return 0;
    return AAudioStream_getFramesWritten(stream_);
}

bool AAudioSink::presentedFrames(int64_t& frames, int64_t& atNanos) const {
    if (!stream_) return false;
    int64_t pos = 0, nanos = 0;
    // CLOCK_MONOTONIC, so the answer is comparable with std::chrono's
    // steady_clock on this platform. The other choice AAudio offers is
    // CLOCK_BOOTTIME, which keeps counting through suspend and is therefore
    // the wrong basis for "how long ago was that".
    if (AAudioStream_getTimestamp(stream_, CLOCK_MONOTONIC, &pos, &nanos) != AAUDIO_OK)
        return false;
    frames  = pos;
    atNanos = nanos;
    return true;
}

void AAudioSink::closeStream() {
    if (stream_) {
        // Discard before stopping, for the same reason stop() does: a bare
        // requestStop() plays the buffer out first, and configure() calls this
        // to REPLACE the stream — so draining here means the old format's tail
        // is heard after the listener already asked for something else.
        pauseAndDiscard();
        AAudioStream_requestStop(stream_);
        AAudioStream_close(stream_);
        stream_ = nullptr;
    }
}

} // namespace ae
