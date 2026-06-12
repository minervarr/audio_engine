package com.nerio.audioengine;

import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;

/**
 * Incremental RIFF/WAVE writer shared by the capture sources. Writes the
 * header on open, appends PCM via {@link #write}, and patches the size
 * fields on {@link #close()}.
 *
 * <p>Integer PCM produces the canonical 44-byte format-tag-1 header.
 * Float PCM (format tag 3, 32-bit IEEE little-endian) carries the
 * non-PCM-required cbSize extension and {@code fact} chunk, so the data
 * chunk starts at byte 58 instead of 44.
 */
final class WavFileWriter {

    private final RandomAccessFile file;
    private final boolean floatFormat;
    private final int blockAlign;
    private long pcmBytes;

    WavFileWriter(File out, int rate, int channels, int bitDepth, boolean floatFormat)
            throws IOException {
        this.floatFormat = floatFormat;
        int bytesPerSample = (bitDepth + 7) / 8;
        this.blockAlign = channels * bytesPerSample;
        int byteRate = rate * blockAlign;

        file = new RandomAccessFile(out, "rw");
        file.setLength(0);
        file.writeBytes("RIFF");
        writeIntLE(file, 0);                  // [4..7] ChunkSize (patched on close)
        file.writeBytes("WAVE");
        file.writeBytes("fmt ");
        writeIntLE(file, floatFormat ? 18 : 16);
        writeShortLE(file, (short) (floatFormat ? 3 : 1)); // 1 = PCM, 3 = IEEE float
        writeShortLE(file, (short) channels);
        writeIntLE(file, rate);
        writeIntLE(file, byteRate);
        writeShortLE(file, (short) blockAlign);
        writeShortLE(file, (short) bitDepth);
        if (floatFormat) {
            writeShortLE(file, (short) 0);    // cbSize = 0
            file.writeBytes("fact");
            writeIntLE(file, 4);
            writeIntLE(file, 0);              // [46..49] sample frames (patched on close)
        }
        file.writeBytes("data");
        writeIntLE(file, 0);                  // data size (patched on close)
    }

    void write(byte[] buf, int offset, int len) throws IOException {
        file.write(buf, offset, len);
        pcmBytes += len;
    }

    long getPcmBytes() {
        return pcmBytes;
    }

    /** Patches the RIFF/fact/data size fields and closes the file. */
    void close() throws IOException {
        int headerAfterRiff = floatFormat ? 50 : 36; // bytes between RIFF size field and data
        long riffSize = Math.min(headerAfterRiff + pcmBytes, 0x7FFFFFFFL);
        long dataSize = Math.min(pcmBytes, 0x7FFFFFFFL);
        file.seek(4);
        writeIntLE(file, (int) riffSize);
        if (floatFormat) {
            long frames = blockAlign > 0 ? Math.min(pcmBytes / blockAlign, 0x7FFFFFFFL) : 0;
            file.seek(46);
            writeIntLE(file, (int) frames);
            file.seek(54);
        } else {
            file.seek(40);
        }
        writeIntLE(file, (int) dataSize);
        file.close();
    }

    private static void writeIntLE(RandomAccessFile f, int v) throws IOException {
        f.write(v & 0xFF);
        f.write((v >>> 8) & 0xFF);
        f.write((v >>> 16) & 0xFF);
        f.write((v >>> 24) & 0xFF);
    }

    private static void writeShortLE(RandomAccessFile f, short v) throws IOException {
        f.write(v & 0xFF);
        f.write((v >>> 8) & 0xFF);
    }
}
