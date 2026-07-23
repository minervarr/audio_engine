#include "dff_parser.h"

#include <unistd.h>
#include <algorithm>
#include <cstring>

namespace ae {

namespace {
// pread the exact byte count at an absolute offset. Returns false on short read.
bool readAt(int fd, int64_t off, void* buf, size_t n) {
    size_t got = 0;
    auto* p = static_cast<uint8_t*>(buf);
    while (got < n) {
        ssize_t r = ::pread(fd, p + got, n - got, off + (int64_t)got);
        if (r <= 0) return false;
        got += (size_t)r;
    }
    return true;
}
bool readId(int fd, int64_t off, char id[4]) { return readAt(fd, off, id, 4); }
bool eq4(const char* a, const char* b) { return std::memcmp(a, b, 4) == 0; }

bool readBE64(int fd, int64_t off, int64_t& out) {
    uint8_t b[8];
    if (!readAt(fd, off, b, 8)) return false;
    out = 0;
    for (int i = 0; i < 8; i++) out = (out << 8) | b[i];
    return true;
}
bool readBE32(int fd, int64_t off, int& out) {
    uint8_t b[4];
    if (!readAt(fd, off, b, 4)) return false;
    out = (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3];
    return true;
}
bool readBE16(int fd, int64_t off, int& out) {
    uint8_t b[2];
    if (!readAt(fd, off, b, 2)) return false;
    out = (b[0] << 8) | b[1];
    return true;
}
} // namespace

bool DffParser::parse(int fd) {
    fd_ = fd;
    char id[4];
    if (!readId(fd, 0, id) || !eq4(id, "FRM8")) return false;
    int64_t frm8Size;
    if (!readBE64(fd, 4, frm8Size)) return false;
    if (!readId(fd, 12, id) || !eq4(id, "DSD ")) return false;

    int64_t pos = 16;
    int64_t endPos = 12 + frm8Size;
    while (pos < endPos) {
        char chunkId[4];
        int64_t chunkSize;
        if (!readId(fd, pos, chunkId)) break;
        if (!readBE64(fd, pos + 4, chunkSize)) break;
        int64_t chunkDataStart = pos + 12;

        if (eq4(chunkId, "PROP")) {
            char propType[4];
            readId(fd, chunkDataStart, propType);   // "SND "
            int64_t p = chunkDataStart + 4;
            int64_t e = chunkDataStart + chunkSize;
            while (p < e) {
                char subId[4];
                int64_t subSize;
                if (!readId(fd, p, subId)) break;
                if (!readBE64(fd, p + 4, subSize)) break;
                int64_t subDataStart = p + 12;
                if (eq4(subId, "FS  ")) {
                    readBE32(fd, subDataStart, sampleRate_);
                } else if (eq4(subId, "CHNL")) {
                    readBE16(fd, subDataStart, channelCount_);
                }
                p = subDataStart + subSize;
                if (p % 2 != 0) p++;
            }
        } else if (eq4(chunkId, "DSD ")) {
            dataOffset_ = chunkDataStart;
            dataSize_ = chunkSize;
        }
        pos = chunkDataStart + chunkSize;
        if (pos % 2 != 0) pos++;
    }

    if (dataOffset_ == 0 || sampleRate_ == 0 || channelCount_ == 0) return false;

    blockSizePerChannel_ = kBlockSize;
    int64_t bytesPerChannel = dataSize_ / channelCount_;
    totalSamples_ = bytesPerChannel * 8;
    totalBlocks_ = (int)((bytesPerChannel + blockSizePerChannel_ - 1) / blockSizePerChannel_);
    currentBlock_ = 0;
    return true;
}

bool DffParser::readBlockPair(uint8_t* leftBlock, uint8_t* rightBlock) {
    if (currentBlock_ >= totalBlocks_) return false;

    int64_t blockStart = dataOffset_
        + (int64_t)currentBlock_ * blockSizePerChannel_ * channelCount_;
    int64_t remaining = dataOffset_ + dataSize_ - blockStart;
    int interleavedSize = (int)std::min(
        (int64_t)blockSizePerChannel_ * channelCount_, remaining);
    int perChannel = interleavedSize / channelCount_;
    if (perChannel <= 0) return false;

    uint8_t interleaved[kBlockSize * 8];  // blockSize * max channels headroom
    if (interleavedSize > (int)sizeof(interleaved)) return false;
    if (!readAt(fd_, blockStart, interleaved, interleavedSize)) return false;

    for (int i = 0; i < perChannel; i++) {
        leftBlock[i] = interleaved[i * channelCount_];
        rightBlock[i] = (channelCount_ >= 2) ? interleaved[i * channelCount_ + 1]
                                             : leftBlock[i];
    }
    if (perChannel < blockSizePerChannel_) {
        std::memset(leftBlock + perChannel, 0, blockSizePerChannel_ - perChannel);
        std::memset(rightBlock + perChannel, 0, blockSizePerChannel_ - perChannel);
    }
    currentBlock_++;
    return true;
}

void DffParser::seekToBlock(int blockIndex) {
    if (blockIndex < 0) blockIndex = 0;
    if (blockIndex >= totalBlocks_) blockIndex = totalBlocks_ - 1;
    currentBlock_ = blockIndex;
}

} // namespace ae
