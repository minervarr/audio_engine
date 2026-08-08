#include "mp3_decoder.h"

#include <sys/stat.h>
#include <unistd.h>
#include "core/os_pread.h"
#include <algorithm>
#include <mutex>

#include <mpg123.h>

namespace ae {

// mh_ is held as void* in the header (keeps it mpg123-free). Cast back here.
static inline mpg123_handle* M(void* p) {
    return static_cast<mpg123_handle*>(p);
}

// mpg123_init() is process-global; run it exactly once.
static void ensureMpg123Init() {
    static std::once_flag once;
    std::call_once(once, [] { mpg123_init(); });
}

// --- libmpg123 reader trampolines (iohandle == Mp3Decoder*) ------------------

static mpg123_ssize_t read_tr(void* h, void* buf, size_t count) {
    return (mpg123_ssize_t)static_cast<Mp3Decoder*>(h)->onRead(buf, (unsigned long)count);
}
static off_t lseek_tr(void* h, off_t offset, int whence) {
    return (off_t)static_cast<Mp3Decoder*>(h)->onSeek((long)offset, whence);
}

// --- Mp3Decoder --------------------------------------------------------------

Mp3Decoder::~Mp3Decoder() { close(); }

bool Mp3Decoder::open(int fd, int64_t offset, int64_t length) {
    close();
    ensureMpg123Init();

    dupFd_ = ::dup(fd);
    if (dupFd_ < 0) return false;

    regionOffset_ = offset < 0 ? 0 : offset;
    if (length < 0) {
        struct stat st;
        regionLength_ = (::fstat(dupFd_, &st) == 0) ? (st.st_size - regionOffset_) : 0;
    } else {
        regionLength_ = length;
    }
    pos_ = 0;
    eos_ = false;

    mh_ = mpg123_new(nullptr, nullptr);
    if (!mh_) { close(); return false; }

    // Quiet, and don't let mpg123 resample or resync noisily.
    mpg123_param(M(mh_), MPG123_ADD_FLAGS, MPG123_QUIET, 0.0);

    // Force interleaved signed-16 output at whatever native rate/channels the
    // stream carries (MP3 is ~16-bit lossy; this keeps the sink path simple).
    const long* rates = nullptr; size_t nrates = 0;
    mpg123_rates(&rates, &nrates);
    mpg123_format_none(M(mh_));
    for (size_t i = 0; i < nrates; i++)
        mpg123_format(M(mh_), rates[i], MPG123_MONO | MPG123_STEREO, MPG123_ENC_SIGNED_16);

    if (mpg123_replace_reader_handle(M(mh_), read_tr, lseek_tr, nullptr) != MPG123_OK) {
        close(); return false;
    }
    if (mpg123_open_handle(M(mh_), this) != MPG123_OK) { close(); return false; }

    // Decode ahead to the first header so format() is valid before read().
    long rate = 0; int channels = 0, enc = 0;
    if (mpg123_getformat(M(mh_), &rate, &channels, &enc) != MPG123_OK) {
        close(); return false;
    }
    mpg123_format_none(M(mh_));
    mpg123_format(M(mh_), rate, MPG123_MONO | MPG123_STEREO, MPG123_ENC_SIGNED_16);
    refreshFormat();
    return format_.valid();
}

void Mp3Decoder::refreshFormat() {
    long rate = 0; int channels = 0, enc = 0;
    if (mpg123_getformat(M(mh_), &rate, &channels, &enc) != MPG123_OK) return;
    format_.sampleRate   = (int)rate;
    format_.channels     = channels;
    format_.bitDepth     = 16;
    format_.subslotBytes = 2;
    format_.isFloat      = false;
    format_.isDsd        = false;
}

int Mp3Decoder::read(uint8_t* out, int maxLen) {
    if (eos_) return -1;
    size_t done = 0;
    int rc = mpg123_read(M(mh_), out, (size_t)maxLen, &done);
    if (rc == MPG123_NEW_FORMAT) {
        refreshFormat();               // rate/channels changed mid-stream
        return (int)done;              // may be 0 -> engine calls again
    }
    if (rc == MPG123_DONE) {
        eos_ = true;
        return done > 0 ? (int)done : -1;
    }
    if (rc != MPG123_OK) {
        // NEED_MORE with a blocking reader means genuine end/short read.
        if (rc == MPG123_NEED_MORE) { eos_ = true; return done > 0 ? (int)done : -1; }
        return done > 0 ? (int)done : -2;   // hard decode error
    }
    return (int)done;
}

int64_t Mp3Decoder::durationMs() const {
    if (!mh_ || format_.sampleRate <= 0) return -1;
    off_t samples = mpg123_length(M(mh_));   // uses Xing/Info header when present
    if (samples <= 0) return -1;
    return (int64_t)samples * 1000 / format_.sampleRate;
}

bool Mp3Decoder::seekMs(int64_t positionMs) {
    if (!mh_ || format_.sampleRate <= 0) return false;
    if (positionMs < 0) positionMs = 0;
    off_t sample = (off_t)(positionMs * format_.sampleRate / 1000);
    if (mpg123_seek(M(mh_), sample, SEEK_SET) < 0) return false;
    eos_ = false;
    return true;
}

void Mp3Decoder::close() {
    if (mh_) {
        mpg123_close(M(mh_));
        mpg123_delete(M(mh_));
        mh_ = nullptr;
    }
    if (dupFd_ >= 0) { ::close(dupFd_); dupFd_ = -1; }
    pos_ = 0;
    eos_ = false;
}

// --- fd-region reader callbacks ----------------------------------------------

long Mp3Decoder::onRead(void* buffer, unsigned long count) {
    int64_t remaining = regionLength_ - pos_;
    if (remaining <= 0) return 0;                       // EOF
    size_t want = std::min<uint64_t>(count, (uint64_t)remaining);
    ssize_t got = osPread(dupFd_, buffer, want, regionOffset_ + pos_);
    if (got < 0)  return -1;
    if (got == 0) return 0;
    pos_ += got;
    return (long)got;
}

long Mp3Decoder::onSeek(long offset, int whence) {
    int64_t base = (whence == SEEK_CUR) ? pos_
                 : (whence == SEEK_END) ? regionLength_
                 : 0;                                   // SEEK_SET
    int64_t np = base + offset;
    if (np < 0) return -1;
    if (np > regionLength_) np = regionLength_;
    pos_ = np;
    return (long)pos_;
}

} // namespace ae
