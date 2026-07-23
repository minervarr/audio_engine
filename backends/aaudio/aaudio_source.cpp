#include "aaudio_source.h"

#include <aaudio/AAudio.h>
#include <android/log.h>

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "AAudioSource", __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "AAudioSource", __VA_ARGS__)

namespace ae {

AAudioSource::~AAudioSource() { closeStream(); }

bool AAudioSource::configure(const AudioFormat& fmt) {
    closeStream();

    AAudioStreamBuilder* b = nullptr;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK || !b) {
        LOGE("createStreamBuilder failed");
        return false;
    }
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_INPUT);
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

    format_.sampleRate   = AAudioStream_getSampleRate(stream_);
    format_.channels     = AAudioStream_getChannelCount(stream_);
    format_.bitDepth     = 16;
    format_.subslotBytes = 2;
    format_.isFloat      = false;
    LOGI("configured: %d Hz, %d ch", format_.sampleRate, format_.channels);
    return format_.valid();
}

bool AAudioSource::start() {
    return stream_ && AAudioStream_requestStart(stream_) == AAUDIO_OK;
}

int AAudioSource::read(uint8_t* out, int maxLen) {
    if (!stream_) return -1;
    const int frameBytes = format_.frameBytes();
    if (frameBytes <= 0) return -1;
    int32_t numFrames = maxLen / frameBytes;
    if (numFrames <= 0) return 0;

    // Blocking read with a 100 ms cap so the engine's stop() stays responsive.
    aaudio_result_t r = AAudioStream_read(stream_, out, numFrames, 100LL * 1000 * 1000);
    if (r < 0) {
        LOGE("read failed: %s", AAudio_convertResultToText(r));
        return -1;
    }
    return r * frameBytes;
}

void AAudioSource::stop() {
    if (stream_) AAudioStream_requestStop(stream_);
}

void AAudioSource::closeStream() {
    if (stream_) {
        AAudioStream_requestStop(stream_);
        AAudioStream_close(stream_);
        stream_ = nullptr;
    }
}

} // namespace ae
