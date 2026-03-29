package com.nerio.audioengine;

import java.io.IOException;
import java.io.RandomAccessFile;

/**
 * Parser for Sony DSF (DSD Stream File) format.
 * Structure: DSD chunk (header) + fmt chunk (format) + data chunk (audio).
 * All fields are little-endian. Chunk sizes include the 12-byte chunk header.
 * Audio data is planar per-channel blocks: [blockSize L][blockSize R][blockSize L]...
 */
public class DsfParser {

    private RandomAccessFile raf;
    private int sampleRate;
    private int channelCount;
    private int bitsPerSample;     // 1=LSB-first, 8=MSB-first
    private int blockSizePerChannel;
    private long totalSamples;     // per channel
    private long dataOffset;       // file position of first audio byte
    private int totalBlocks;
    private int currentBlock;

    public void parse(RandomAccessFile raf) throws IOException {
        this.raf = raf;
        raf.seek(0);

        // DSD chunk: "DSD " + chunkSize(8) + totalFileSize(8) + metadataOffset(8)
        readMagic("DSD ");
        long dsdChunkSize = readLE64();
        long totalFileSize = readLE64();
        long metadataOffset = readLE64();

        // fmt chunk starts right after DSD chunk
        raf.seek(dsdChunkSize);
        readMagic("fmt ");
        long fmtChunkSize = readLE64();
        int formatVersion = readLE32();
        int formatId = readLE32();
        int channelType = readLE32();
        channelCount = readLE32();
        sampleRate = readLE32();
        bitsPerSample = readLE32();
        totalSamples = readLE64();
        blockSizePerChannel = readLE32();
        int reserved = readLE32();

        // data chunk starts after fmt chunk
        raf.seek(dsdChunkSize + fmtChunkSize);
        readMagic("data");
        long dataChunkSize = readLE64();
        dataOffset = raf.getFilePointer();

        // Compute total block pairs
        if (blockSizePerChannel > 0) {
            long samplesPerBlock = (long) blockSizePerChannel * 8;
            totalBlocks = (int) ((totalSamples + samplesPerBlock - 1) / samplesPerBlock);
        }
        currentBlock = 0;
    }

    /**
     * Reads one L+R block pair. DSF stores planar blocks:
     * [blockSizePerChannel bytes L][blockSizePerChannel bytes R]
     * Returns false at end of data.
     */
    public boolean readBlockPair(byte[] leftBlock, byte[] rightBlock) throws IOException {
        if (currentBlock >= totalBlocks) return false;

        long blockOffset = dataOffset
                + (long) currentBlock * blockSizePerChannel * channelCount;
        raf.seek(blockOffset);

        raf.readFully(leftBlock, 0, blockSizePerChannel);
        if (channelCount >= 2) {
            raf.readFully(rightBlock, 0, blockSizePerChannel);
        } else {
            System.arraycopy(leftBlock, 0, rightBlock, 0, blockSizePerChannel);
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
    public int getBitsPerSample() { return bitsPerSample; }
    public int getBlockSizePerChannel() { return blockSizePerChannel; }
    public long getTotalSamples() { return totalSamples; }
    public int getTotalBlocks() { return totalBlocks; }

    private void readMagic(String expected) throws IOException {
        byte[] b = new byte[4];
        raf.readFully(b);
        String got = new String(b, "US-ASCII");
        if (!expected.equals(got)) {
            throw new IOException("Expected '" + expected + "', got '" + got + "'");
        }
    }

    private long readLE64() throws IOException {
        byte[] b = new byte[8];
        raf.readFully(b);
        return (b[0] & 0xFFL)
                | ((b[1] & 0xFFL) << 8)
                | ((b[2] & 0xFFL) << 16)
                | ((b[3] & 0xFFL) << 24)
                | ((b[4] & 0xFFL) << 32)
                | ((b[5] & 0xFFL) << 40)
                | ((b[6] & 0xFFL) << 48)
                | ((b[7] & 0xFFL) << 56);
    }

    private int readLE32() throws IOException {
        byte[] b = new byte[4];
        raf.readFully(b);
        return (b[0] & 0xFF)
                | ((b[1] & 0xFF) << 8)
                | ((b[2] & 0xFF) << 16)
                | ((b[3] & 0xFF) << 24);
    }
}
