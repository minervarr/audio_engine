package com.nerio.audioengine;

import java.io.IOException;
import java.io.RandomAccessFile;
import java.util.Arrays;

/**
 * Parser for Philips DSDIFF (.dff) format.
 * Structure: FRM8 container with PROP (properties) and DSD (audio data) chunks.
 * All fields are big-endian. Chunk sizes do NOT include the 12-byte chunk header.
 * Audio data is byte-interleaved: [1 byte L][1 byte R][1 byte L][1 byte R]...
 * Always MSB-first (no bit-reversal needed).
 */
public class DffParser {

    private static final int BLOCK_SIZE = 4096;

    private RandomAccessFile raf;
    private int sampleRate;
    private int channelCount;
    private long dataOffset;
    private long dataSize;
    private long totalSamples;    // per channel
    private int blockSizePerChannel;
    private int totalBlocks;
    private int currentBlock;

    public void parse(RandomAccessFile raf) throws IOException {
        this.raf = raf;
        raf.seek(0);

        // FRM8 header
        String id = readChunkId();
        if (!"FRM8".equals(id)) throw new IOException("Not a DFF file");
        long frm8Size = readBE64();
        String formType = readChunkId();
        if (!"DSD ".equals(formType)) throw new IOException("Not DSD form type");

        // Scan sub-chunks within FRM8
        // Sub-chunks start after: "FRM8"(4) + size(8) + "DSD "(4) = offset 16
        long pos = 16;
        long endPos = 12 + frm8Size;  // 4 (ID) + 8 (size) + data

        while (pos < endPos) {
            raf.seek(pos);
            String chunkId = readChunkId();
            long chunkSize = readBE64();
            long chunkDataStart = pos + 12;

            if ("PROP".equals(chunkId)) {
                parsePropChunk(chunkDataStart, chunkSize);
            } else if ("DSD ".equals(chunkId)) {
                dataOffset = chunkDataStart;
                dataSize = chunkSize;
            }

            // Advance to next chunk, pad to even boundary per DSDIFF spec
            pos = chunkDataStart + chunkSize;
            if (pos % 2 != 0) pos++;
        }

        if (dataOffset == 0 || sampleRate == 0 || channelCount == 0) {
            throw new IOException("Missing required DFF chunks");
        }

        blockSizePerChannel = BLOCK_SIZE;
        long bytesPerChannel = dataSize / channelCount;
        totalSamples = bytesPerChannel * 8;
        totalBlocks = (int) ((bytesPerChannel + blockSizePerChannel - 1) / blockSizePerChannel);
        currentBlock = 0;
    }

    private void parsePropChunk(long dataStart, long chunkDataSize) throws IOException {
        raf.seek(dataStart);
        String propType = readChunkId(); // "SND "

        long pos = dataStart + 4;
        long endPos = dataStart + chunkDataSize;

        while (pos < endPos) {
            raf.seek(pos);
            String subId = readChunkId();
            long subSize = readBE64();
            long subDataStart = pos + 12;

            if ("FS  ".equals(subId)) {
                raf.seek(subDataStart);
                sampleRate = readBE32();
            } else if ("CHNL".equals(subId)) {
                raf.seek(subDataStart);
                channelCount = readBE16();
            }

            pos = subDataStart + subSize;
            if (pos % 2 != 0) pos++;
        }
    }

    /**
     * Reads one block pair by deinterleaving byte-interleaved DFF audio
     * into separate L and R buffers. Returns false at end of data.
     */
    public boolean readBlockPair(byte[] leftBlock, byte[] rightBlock) throws IOException {
        if (currentBlock >= totalBlocks) return false;

        long blockStart = dataOffset
                + (long) currentBlock * blockSizePerChannel * channelCount;

        long remaining = dataOffset + dataSize - blockStart;
        int interleavedSize = (int) Math.min(
                (long) blockSizePerChannel * channelCount, remaining);
        int perChannel = interleavedSize / channelCount;

        if (perChannel <= 0) return false;

        byte[] interleaved = new byte[interleavedSize];
        raf.seek(blockStart);
        raf.readFully(interleaved, 0, interleavedSize);

        // Deinterleave [L][R][L][R]... into separate blocks
        for (int i = 0; i < perChannel; i++) {
            leftBlock[i] = interleaved[i * channelCount];
            if (channelCount >= 2) {
                rightBlock[i] = interleaved[i * channelCount + 1];
            } else {
                rightBlock[i] = leftBlock[i];
            }
        }

        // Zero-fill remainder of last partial block
        if (perChannel < blockSizePerChannel) {
            Arrays.fill(leftBlock, perChannel, blockSizePerChannel, (byte) 0);
            Arrays.fill(rightBlock, perChannel, blockSizePerChannel, (byte) 0);
        }

        currentBlock++;
        return true;
    }

    public void seekToBlock(int blockIndex) {
        if (blockIndex < 0) blockIndex = 0;
        if (blockIndex >= totalBlocks) blockIndex = totalBlocks - 1;
        currentBlock = blockIndex;
    }

    public int getSampleRate() { return sampleRate; }
    public int getChannelCount() { return channelCount; }
    public int getBlockSizePerChannel() { return blockSizePerChannel; }
    public long getTotalSamples() { return totalSamples; }
    public int getTotalBlocks() { return totalBlocks; }

    private String readChunkId() throws IOException {
        byte[] b = new byte[4];
        raf.readFully(b);
        return new String(b, "US-ASCII");
    }

    private long readBE64() throws IOException {
        byte[] b = new byte[8];
        raf.readFully(b);
        return ((b[0] & 0xFFL) << 56)
                | ((b[1] & 0xFFL) << 48)
                | ((b[2] & 0xFFL) << 40)
                | ((b[3] & 0xFFL) << 32)
                | ((b[4] & 0xFFL) << 24)
                | ((b[5] & 0xFFL) << 16)
                | ((b[6] & 0xFFL) << 8)
                | (b[7] & 0xFFL);
    }

    private int readBE32() throws IOException {
        byte[] b = new byte[4];
        raf.readFully(b);
        return ((b[0] & 0xFF) << 24)
                | ((b[1] & 0xFF) << 16)
                | ((b[2] & 0xFF) << 8)
                | (b[3] & 0xFF);
    }

    private int readBE16() throws IOException {
        byte[] b = new byte[2];
        raf.readFully(b);
        return ((b[0] & 0xFF) << 8) | (b[1] & 0xFF);
    }
}
