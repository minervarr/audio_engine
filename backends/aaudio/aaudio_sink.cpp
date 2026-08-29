#include "aaudio_sink.h"

#include <aaudio/AAudio.h>
#include <android/log.h>
#include <time.h>

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "AAudioSink", __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "AAudioSink", __VA_ARGS__)

namespace ae {

AAudioSink::~AAudioSink() { closeStream(); }

bool AAudioSink::configure(const AudioFormat& fmt) {
    closeStream();
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
        if (r < 0) { LOGE("write failed: %s", AAudio_convertResultToText(r)); return -1; }
        return (int)r * srcFrameBytes;   // report source-domain bytes consumed
    }

    // 16-bit passthrough. Blocking write with a 100 ms cap so stop() stays responsive.
    const int frameBytes = 2 * channels;
    int32_t numFrames = len / frameBytes;
    if (numFrames <= 0) return 0;
    aaudio_result_t r = AAudioStream_write(stream_, data, numFrames, 100LL * 1000 * 1000);
    if (r < 0) {
        LOGE("write failed: %s", AAudio_convertResultToText(r));
        return -1;
    }
    return (int)r * frameBytes;
}

void AAudioSink::pause() {
    if (stream_) AAudioStream_requestPause(stream_);
}

void AAudioSink::resume() {
    if (stream_) AAudioStream_requestStart(stream_);
}

void AAudioSink::flush() {
    // AAudio requires the stream be paused/stopped before flushing.
    if (stream_) AAudioStream_requestFlush(stream_);
}

void AAudioSink::stop() {
    if (stream_) AAudioStream_requestStop(stream_);
}

int AAudioSink::pendingPlaybackMs() const {
    if (!stream_ || format_.sampleRate <= 0) return 0;
    int64_t written = AAudioStream_getFramesWritten(stream_);
    int64_t read    = AAudioStream_getFramesRead(stream_);
    int64_t pending = written - read;
    if (pending < 0) pending = 0;
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
        AAudioStream_requestStop(stream_);
        AAudioStream_close(stream_);
        stream_ = nullptr;
    }
}

} // namespace ae
