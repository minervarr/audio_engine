#pragma once
// Minimal RIFF/WAVE PCM writer for the smoke-test tools. Write the header
// with placeholder sizes, stream raw PCM after it, then patch the sizes on
// close. Integer PCM only (16/24/32-bit), matching what the capture
// backends deliver.

#include <cstdint>
#include <cstdio>

class WavWriter {
public:
    bool open(const char* path, int sampleRate, int channels, int bitDepth) {
        f_ = fopen(path, "wb");
        if (!f_) return false;
        sampleRate_ = sampleRate;
        channels_ = channels;
        bitDepth_ = bitDepth;
        dataBytes_ = 0;
        writeHeader();
        return true;
    }

    void write(const uint8_t* data, size_t len) {
        if (!f_) return;
        fwrite(data, 1, len, f_);
        dataBytes_ += len;
    }

    void close() {
        if (!f_) return;
        // Patch RIFF chunk size (offset 4) and data chunk size (offset 40).
        uint32_t riffSize = 36 + (uint32_t)dataBytes_;
        uint32_t dataSize = (uint32_t)dataBytes_;
        fseek(f_, 4, SEEK_SET);
        fwrite(&riffSize, 4, 1, f_);
        fseek(f_, 40, SEEK_SET);
        fwrite(&dataSize, 4, 1, f_);
        fclose(f_);
        f_ = nullptr;
    }

    uint64_t bytesWritten() const { return dataBytes_; }

private:
    void writeHeader() {
        uint16_t fmt = 1; // PCM integer
        uint16_t blockAlign = (uint16_t)(channels_ * bitDepth_ / 8);
        uint32_t byteRate = (uint32_t)sampleRate_ * blockAlign;
        uint32_t zero = 0;
        uint16_t bits = (uint16_t)bitDepth_;
        uint16_t ch = (uint16_t)channels_;
        uint32_t rate = (uint32_t)sampleRate_;
        uint32_t fmtSize = 16;
        fwrite("RIFF", 1, 4, f_);
        fwrite(&zero, 4, 1, f_);          // patched in close()
        fwrite("WAVE", 1, 4, f_);
        fwrite("fmt ", 1, 4, f_);
        fwrite(&fmtSize, 4, 1, f_);
        fwrite(&fmt, 2, 1, f_);
        fwrite(&ch, 2, 1, f_);
        fwrite(&rate, 4, 1, f_);
        fwrite(&byteRate, 4, 1, f_);
        fwrite(&blockAlign, 2, 1, f_);
        fwrite(&bits, 2, 1, f_);
        fwrite("data", 1, 4, f_);
        fwrite(&zero, 4, 1, f_);          // patched in close()
    }

    FILE* f_ = nullptr;
    int sampleRate_ = 0, channels_ = 0, bitDepth_ = 0;
    uint64_t dataBytes_ = 0;
};
