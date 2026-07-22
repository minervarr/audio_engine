#pragma once
// Minimal RIFF/WAVE PCM reader for the play smoke tools. Parses the fmt chunk
// (rate / channels / bits / format tag), then streams raw PCM from the data
// chunk. Integer PCM (tag 1) and IEEE float (tag 3); enough for the tools.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>

class WavReader {
public:
    ~WavReader() { close(); }

    bool open(const char* path) {
        f_ = fopen(path, "rb");
        if (!f_) return false;

        char riff[4], wave[4];
        uint32_t riffSz = 0;
        if (fread(riff, 1, 4, f_) != 4 || fread(&riffSz, 4, 1, f_) != 1 ||
            fread(wave, 1, 4, f_) != 4 ||
            memcmp(riff, "RIFF", 4) != 0 || memcmp(wave, "WAVE", 4) != 0) {
            close();
            return false;
        }

        char id[4];
        uint32_t csz = 0;
        while (fread(id, 1, 4, f_) == 4 && fread(&csz, 4, 1, f_) == 1) {
            if (memcmp(id, "fmt ", 4) == 0) {
                uint16_t fmt = 0, ch = 0, blockAlign = 0, bits = 0;
                uint32_t rate = 0, byteRate = 0;
                fread(&fmt, 2, 1, f_);        fread(&ch, 2, 1, f_);
                fread(&rate, 4, 1, f_);       fread(&byteRate, 4, 1, f_);
                fread(&blockAlign, 2, 1, f_); fread(&bits, 2, 1, f_);
                formatTag_  = fmt;
                channels_   = ch;
                sampleRate_ = (int)rate;
                bitDepth_   = bits;
                if (csz > 16) fseek(f_, (long)(csz - 16), SEEK_CUR);  // skip cbSize/extensible
            } else if (memcmp(id, "data", 4) == 0) {
                dataRemaining_ = csz;
                haveData_ = true;
                break;
            } else {
                fseek(f_, (long)(csz + (csz & 1)), SEEK_CUR);         // skip, pad to even
            }
        }
        return haveData_ && sampleRate_ > 0 && channels_ > 0 && bitDepth_ > 0;
    }

    // Reads up to maxBytes of raw PCM. Returns bytes read, 0 at end of data.
    int read(uint8_t* out, int maxBytes) {
        if (!f_ || dataRemaining_ == 0 || maxBytes <= 0) return 0;
        size_t want = std::min((size_t)maxBytes, dataRemaining_);
        size_t n = fread(out, 1, want, f_);
        dataRemaining_ -= n;
        return (int)n;
    }

    int  sampleRate() const { return sampleRate_; }
    int  channels()   const { return channels_; }
    int  bitDepth()   const { return bitDepth_; }
    int  formatTag()  const { return formatTag_; }   // 1 = integer PCM, 3 = IEEE float
    bool isFloat()    const { return formatTag_ == 3; }

    void close() {
        if (f_) { fclose(f_); f_ = nullptr; }
    }

private:
    FILE* f_ = nullptr;
    int sampleRate_ = 0, channels_ = 0, bitDepth_ = 0, formatTag_ = 0;
    size_t dataRemaining_ = 0;
    bool haveData_ = false;
};
