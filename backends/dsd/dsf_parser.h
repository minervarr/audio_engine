#ifndef AE_BACKENDS_DSD_DSF_PARSER_H
#define AE_BACKENDS_DSD_DSF_PARSER_H

#include <cstdint>

namespace ae {

// Parser for Sony DSF (.dsf): DSD + fmt + data chunks (little-endian). Audio is
// planar per-channel blocks [blockSize L][blockSize R]…. bitsPerSample==1 means
// LSB-first (the decoder bit-reverses); 8 means MSB-first. Reads via pread on a
// caller-owned fd. Ported from the Java DsfParser.
class DsfParser {
public:
    bool parse(int fd);

    bool readBlockPair(uint8_t* leftBlock, uint8_t* rightBlock);
    void seekToBlock(int blockIndex);

    int  sampleRate() const          { return sampleRate_; }
    int  channelCount() const        { return channelCount_; }
    int  bitsPerSample() const       { return bitsPerSample_; }
    int  blockSizePerChannel() const { return blockSizePerChannel_; }
    int  totalBlocks() const         { return totalBlocks_; }
    int64_t totalSamples() const     { return totalSamples_; }

private:
    int     fd_ = -1;
    int     sampleRate_ = 0;
    int     channelCount_ = 0;
    int     bitsPerSample_ = 0;
    int     blockSizePerChannel_ = 0;
    int64_t totalSamples_ = 0;
    int64_t dataOffset_ = 0;
    int     totalBlocks_ = 0;
    int     currentBlock_ = 0;
};

} // namespace ae

#endif // AE_BACKENDS_DSD_DSF_PARSER_H
