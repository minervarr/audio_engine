#include "dsf_parser.h"

#include <unistd.h>
#include <cstring>

namespace ae {

namespace {
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
bool magicAt(int fd, int64_t off, const char* expect) {
    char b[4];
    return readAt(fd, off, b, 4) && std::memcmp(b, expect, 4) == 0;
}
bool readLE64(int fd, int64_t off, int64_t& out) {
    uint8_t b[8];
    if (!readAt(fd, off, b, 8)) return false;
    out = 0;
    for (int i = 7; i >= 0; i--) out = (out << 8) | b[i];
    return true;
}
bool readLE32(int fd, int64_t off, int& out) {
    uint8_t b[4];
    if (!readAt(fd, off, b, 4)) return false;
    out = b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24);
    return true;
}
} // namespace

bool DsfParser::parse(int fd) {
    fd_ = fd;
    if (!magicAt(fd, 0, "DSD ")) return false;
    int64_t dsdChunkSize;
    if (!readLE64(fd, 4, dsdChunkSize)) return false;   // then totalFileSize(8), metadataOffset(8)

    // fmt chunk starts right after the DSD chunk.
    int64_t fmtBase = dsdChunkSize;
    if (!magicAt(fd, fmtBase, "fmt ")) return false;
    int64_t fmtChunkSize;
    if (!readLE64(fd, fmtBase + 4, fmtChunkSize)) return false;
    // formatVersion(4), formatId(4), channelType(4) then the fields we need:
    readLE32(fd, fmtBase + 12 + 12, channelCount_);
    readLE32(fd, fmtBase + 12 + 16, sampleRate_);
    readLE32(fd, fmtBase + 12 + 20, bitsPerSample_);
    readLE64(fd, fmtBase + 12 + 24, totalSamples_);
    readLE32(fd, fmtBase + 12 + 32, blockSizePerChannel_);

    // data chunk after fmt chunk; audio begins after "data"(4) + size(8).
    int64_t dataBase = dsdChunkSize + fmtChunkSize;
    if (!magicAt(fd, dataBase, "data")) return false;
    dataOffset_ = dataBase + 12;

    if (channelCount_ <= 0 || sampleRate_ <= 0 || blockSizePerChannel_ <= 0) return false;
    int64_t samplesPerBlock = (int64_t)blockSizePerChannel_ * 8;
    totalBlocks_ = (int)((totalSamples_ + samplesPerBlock - 1) / samplesPerBlock);
    currentBlock_ = 0;
    return true;
}

bool DsfParser::readBlockPair(uint8_t* leftBlock, uint8_t* rightBlock) {
    if (currentBlock_ >= totalBlocks_) return false;

    int64_t blockOffset = dataOffset_
        + (int64_t)currentBlock_ * blockSizePerChannel_ * channelCount_;
    if (!readAt(fd_, blockOffset, leftBlock, blockSizePerChannel_)) return false;
    if (channelCount_ >= 2) {
        if (!readAt(fd_, blockOffset + blockSizePerChannel_, rightBlock,
                    blockSizePerChannel_)) return false;
    } else {
        std::memcpy(rightBlock, leftBlock, blockSizePerChannel_);
    }
    currentBlock_++;
    return true;
}

void DsfParser::seekToBlock(int blockIndex) {
    if (blockIndex < 0) blockIndex = 0;
    if (blockIndex >= totalBlocks_) blockIndex = totalBlocks_ - 1;
    currentBlock_ = blockIndex;
}

} // namespace ae
