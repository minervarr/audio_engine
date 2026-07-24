#include "mp3_encoder.h"

#include <unistd.h>
#include <algorithm>

#include <lame.h>

namespace ae {

// gf_ is held as void* in the header (keeps it lame-free). Cast back here.
static inline lame_global_flags* G(void* p) {
    return static_cast<lame_global_flags*>(p);
}

Mp3Encoder::~Mp3Encoder() { close(); }

bool Mp3Encoder::open(int fd, const AudioFormat& fmt) {
    close();
    if (fmt.channels <= 0 || fmt.sampleRate <= 0) return false;
    channels_ = fmt.channels;

    int dupFd = ::dup(fd);
    if (dupFd < 0) return false;
    file_ = ::fdopen(dupFd, "wb");
    if (!file_) { ::close(dupFd); return false; }

    gf_ = lame_init();
    if (!gf_) { std::fclose(file_); file_ = nullptr; return false; }

    lame_set_num_channels(G(gf_), channels_);
    lame_set_in_samplerate(G(gf_), fmt.sampleRate);
    lame_set_mode(G(gf_), channels_ == 1 ? MONO : JOINT_STEREO);
    lame_set_VBR(G(gf_), vbr_default);        // VBR ~V4; good quality/size default
    lame_set_VBR_q(G(gf_), 2);                // tighten to ~V2 (~190 kbps)
    lame_set_bWriteVbrTag(G(gf_), 1);         // reserve + patch the Xing header

    if (lame_init_params(G(gf_)) < 0) { close(); return false; }
    return true;
}

bool Mp3Encoder::encode(const uint8_t* pcm, int len) {
    if (!gf_ || channels_ <= 0) return false;
    const int frames = (len / 2) / channels_;      // interleaved int16
    if (frames <= 0) return true;

    // LAME worst case: 1.25 * frames + 7200 bytes.
    size_t need = (size_t)(1.25 * frames) + 7200;
    if (mp3buf_.size() < need) mp3buf_.resize(need);

    const int16_t* src = reinterpret_cast<const int16_t*>(pcm);
    int n = lame_encode_buffer_interleaved(
        G(gf_), const_cast<short*>(reinterpret_cast<const short*>(src)),
        frames, mp3buf_.data(), (int)mp3buf_.size());
    if (n < 0) return false;
    if (n > 0 && std::fwrite(mp3buf_.data(), 1, (size_t)n, file_) != (size_t)n) return false;
    return true;
}

void Mp3Encoder::close() {
    if (gf_) {
        if (file_) {
            if (mp3buf_.size() < 7200) mp3buf_.resize(7200);
            int n = lame_encode_flush(G(gf_), mp3buf_.data(), (int)mp3buf_.size());
            if (n > 0) std::fwrite(mp3buf_.data(), 1, (size_t)n, file_);

            // Patch the Xing/LAME VBR header into the reserved first frame.
            size_t tag = lame_get_lametag_frame(G(gf_), nullptr, 0);
            if (tag > 0) {
                if (mp3buf_.size() < tag) mp3buf_.resize(tag);
                tag = lame_get_lametag_frame(G(gf_), mp3buf_.data(), mp3buf_.size());
                if (tag > 0 && std::fseek(file_, 0, SEEK_SET) == 0)
                    std::fwrite(mp3buf_.data(), 1, tag, file_);
            }
        }
        lame_close(G(gf_));
        gf_ = nullptr;
    }
    if (file_) { std::fclose(file_); file_ = nullptr; }   // also closes the dup fd
    channels_ = 0;
}

} // namespace ae
