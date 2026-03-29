#ifndef GAPLESS_DECODER_H
#define GAPLESS_DECODER_H

#include <cstdint>
#include <cstring>
#include "pcm_buffer.h"

class GaplessDecoder {
public:
    GaplessDecoder(int delayFrames, int paddingFrames, int frameSize, NativePcmBuffer* buffer)
        : paddingBytes(paddingFrames * frameSize),
          delayBytesRemaining(delayFrames * frameSize),
          buffer(buffer),
          tailBuf(paddingBytes > 0 ? new uint8_t[paddingBytes] : nullptr),
          tailLen(0) {}

    ~GaplessDecoder() { delete[] tailBuf; }

    // Process one decoded frame. Returns false if buffer write was interrupted (flush).
    bool processFrame(const uint8_t* data, int offset, int length) {
        int dataOffset = offset;
        int dataLength = length;

        // Skip encoder delay at file start
        if (delayBytesRemaining > 0) {
            int toSkip = std::min(dataLength, delayBytesRemaining);
            delayBytesRemaining -= toSkip;
            dataOffset += toSkip;
            dataLength -= toSkip;
        }

        if (dataLength <= 0) return true;

        if (tailBuf != nullptr) {
            // Holdback: keep last paddingBytes in tailBuf, discard on EOS
            int totalAvail = tailLen + dataLength;
            int toOutput = totalAvail - paddingBytes;
            if (toOutput > 0) {
                int fromTail = std::min(toOutput, tailLen);
                if (fromTail > 0) {
                    if (!buffer->write(tailBuf, 0, fromTail)) return false;
                    if (fromTail < tailLen) {
                        memmove(tailBuf, tailBuf + fromTail, tailLen - fromTail);
                    }
                    tailLen -= fromTail;
                    toOutput -= fromTail;
                }
                if (toOutput > 0) {
                    if (!buffer->write(data, dataOffset, toOutput)) return false;
                    dataOffset += toOutput;
                    dataLength -= toOutput;
                }
            }
            if (dataLength > 0) {
                memcpy(tailBuf + tailLen, data + dataOffset, dataLength);
                tailLen += dataLength;
            }
        } else {
            if (!buffer->write(data, dataOffset, dataLength)) return false;
        }
        return true;
    }

    void signalEnd() {
        // Tail buffer contents (padding) are discarded -- that's the gapless trim
        buffer->signalEnd();
    }

    void resetAfterSeek() {
        tailLen = 0;
    }

private:
    const int paddingBytes;
    int delayBytesRemaining;
    NativePcmBuffer* buffer; // not owned
    uint8_t* tailBuf;
    int tailLen;
};

#endif // GAPLESS_DECODER_H
