package com.nerio.audioengine;

public interface AudioOutput {
    boolean configure(int sampleRate, int channelCount, int encoding, int sourceBitDepth);
    boolean start();
    int write(byte[] data, int offset, int length);
    void pause();
    void resume();
    void flush();
    void stop();
    void release();
}
