#include "mediacodec_decoder.h"

#include <sys/stat.h>
#include <unistd.h>
#include <cstring>

#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <android/log.h>

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "MediaCodecDecoder", __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "MediaCodecDecoder", __VA_ARGS__)

namespace ae {

MediaCodecDecoder::~MediaCodecDecoder() { close(); }

bool MediaCodecDecoder::open(int fd, int64_t offset, int64_t length) {
    close();

    // Own our own fd so playback is independent of the caller's handle.
    dupFd_ = ::dup(fd);
    if (dupFd_ < 0) { LOGE("dup(fd) failed"); return false; }

    if (offset < 0) offset = 0;
    if (length < 0) {
        struct stat st;
        if (::fstat(dupFd_, &st) == 0) length = st.st_size - offset;
        else length = INT64_MAX;
    }

    extractor_ = AMediaExtractor_new();
    if (AMediaExtractor_setDataSourceFd(extractor_, dupFd_, offset, length) != AMEDIA_OK) {
        LOGE("setDataSourceFd failed");
        close();
        return false;
    }

    // Select the first audio track.
    size_t tracks = AMediaExtractor_getTrackCount(extractor_);
    AMediaFormat* trackFmt = nullptr;
    const char* mime = nullptr;
    for (size_t i = 0; i < tracks; ++i) {
        AMediaFormat* f = AMediaExtractor_getTrackFormat(extractor_, i);
        const char* m = nullptr;
        if (AMediaFormat_getString(f, AMEDIAFORMAT_KEY_MIME, &m) && m &&
            std::strncmp(m, "audio/", 6) == 0) {
            AMediaExtractor_selectTrack(extractor_, i);
            trackFmt = f;
            mime = m;
            break;
        }
        AMediaFormat_delete(f);
    }
    if (!trackFmt) { LOGE("no audio track"); close(); return false; }

    int32_t rate = 0, channels = 0;
    AMediaFormat_getInt32(trackFmt, AMEDIAFORMAT_KEY_SAMPLE_RATE, &rate);
    AMediaFormat_getInt32(trackFmt, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &channels);
    int64_t durUs = -1;
    if (AMediaFormat_getInt64(trackFmt, AMEDIAFORMAT_KEY_DURATION, &durUs) && durUs >= 0) {
        durationMs_ = durUs / 1000;
    }

    // Gapless trim metadata (MP3/AAC). The string keys work at minSdk 26 without
    // the API-28 AMEDIAFORMAT_KEY_ENCODER_* symbols. Absent for lossless formats.
    int32_t delay = 0, padding = 0;
    if (AMediaFormat_getInt32(trackFmt, "encoder-delay", &delay)   && delay   > 0) delayFrames_   = delay;
    if (AMediaFormat_getInt32(trackFmt, "encoder-padding", &padding) && padding > 0) paddingFrames_ = padding;

    codec_ = AMediaCodec_createDecoderByType(mime);
    if (!codec_) { LOGE("createDecoderByType(%s) failed", mime); AMediaFormat_delete(trackFmt); close(); return false; }
    if (AMediaCodec_configure(codec_, trackFmt, nullptr, nullptr, 0) != AMEDIA_OK) {
        LOGE("codec configure failed"); AMediaFormat_delete(trackFmt); close(); return false;
    }
    AMediaFormat_delete(trackFmt);
    if (AMediaCodec_start(codec_) != AMEDIA_OK) { LOGE("codec start failed"); close(); return false; }

    format_.sampleRate   = rate;
    format_.channels     = channels;
    format_.bitDepth     = 16;
    format_.subslotBytes = 2;
    format_.isFloat      = false;
    LOGI("opened: %d Hz, %d ch, %lld ms", rate, channels, (long long)durationMs_);
    return format_.valid();
}

int MediaCodecDecoder::servePending(uint8_t* out, int maxLen) {
    size_t remain = pending_.size() - pendingOff_;
    int n = (int)std::min<size_t>(remain, (size_t)maxLen);
    std::memcpy(out, pending_.data() + pendingOff_, n);
    pendingOff_ += n;
    if (pendingOff_ >= pending_.size()) { pending_.clear(); pendingOff_ = 0; }
    return n;
}

int MediaCodecDecoder::read(uint8_t* out, int maxLen) {
    if (pendingOff_ < pending_.size()) return servePending(out, maxLen);
    if (outputDone_) return -1;
    if (!codec_) return -2;

    // Feed one input buffer.
    if (!inputDone_) {
        ssize_t inIdx = AMediaCodec_dequeueInputBuffer(codec_, 10000);
        if (inIdx >= 0) {
            size_t cap = 0;
            uint8_t* inBuf = AMediaCodec_getInputBuffer(codec_, inIdx, &cap);
            ssize_t sampleSize = inBuf ? AMediaExtractor_readSampleData(extractor_, inBuf, cap) : -1;
            if (sampleSize < 0) {
                AMediaCodec_queueInputBuffer(codec_, inIdx, 0, 0, 0,
                                             AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
                inputDone_ = true;
            } else {
                int64_t pts = AMediaExtractor_getSampleTime(extractor_);
                AMediaCodec_queueInputBuffer(codec_, inIdx, 0, sampleSize, pts, 0);
                AMediaExtractor_advance(extractor_);
            }
        }
    }

    // Drain one output buffer.
    AMediaCodecBufferInfo info;
    ssize_t outIdx = AMediaCodec_dequeueOutputBuffer(codec_, &info, 10000);
    if (outIdx >= 0) {
        bool eos = (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
        int produced = 0;
        if (info.size > 0) {
            size_t cap = 0;
            uint8_t* outBuf = AMediaCodec_getOutputBuffer(codec_, outIdx, &cap);
            if (outBuf) {
                int n = std::min(info.size, maxLen);
                std::memcpy(out, outBuf + info.offset, n);
                if (n < info.size) {
                    pending_.assign(outBuf + info.offset + n, outBuf + info.offset + info.size);
                    pendingOff_ = 0;
                }
                produced = n;
            }
        }
        AMediaCodec_releaseOutputBuffer(codec_, outIdx, false);
        if (eos) outputDone_ = true;
        if (produced > 0) return produced;
        return outputDone_ ? -1 : 0;
    }
    // INFO_OUTPUT_FORMAT_CHANGED / BUFFERS_CHANGED / TRY_AGAIN_LATER
    if (outIdx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
        AMediaFormat* of = AMediaCodec_getOutputFormat(codec_);
        int32_t rate = format_.sampleRate, ch = format_.channels;
        AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_SAMPLE_RATE, &rate);
        AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &ch);
        format_.sampleRate = rate;
        format_.channels   = ch;
        AMediaFormat_delete(of);
    }
    return 0;
}

bool MediaCodecDecoder::seekMs(int64_t positionMs) {
    if (!extractor_ || !codec_) return false;
    if (positionMs < 0) positionMs = 0;
    // Seek the container to the sync sample at/just before the target, then
    // flush the codec so no pre-seek PCM leaks out. Reset the EOS/pending state.
    AMediaExtractor_seekTo(extractor_, positionMs * 1000,
                           AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC);
    AMediaCodec_flush(codec_);
    pending_.clear();
    pendingOff_ = 0;
    inputDone_  = false;
    outputDone_ = false;
    return true;
}

void MediaCodecDecoder::close() {
    if (codec_)     { AMediaCodec_stop(codec_); AMediaCodec_delete(codec_); codec_ = nullptr; }
    if (extractor_) { AMediaExtractor_delete(extractor_); extractor_ = nullptr; }
    if (dupFd_ >= 0) { ::close(dupFd_); dupFd_ = -1; }
    pending_.clear();
    pendingOff_ = 0;
    inputDone_ = false;
    outputDone_ = false;
}

} // namespace ae
