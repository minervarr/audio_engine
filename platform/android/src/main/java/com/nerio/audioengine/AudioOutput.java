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

    /**
     * Milliseconds of audio already handed to this output but not yet rendered
     * by the device (driver/ring/queue latency). The engine waits for this to
     * reach ~0 at end-of-track before advancing, so a deeply-buffered output
     * (e.g. a USB DAC) doesn't get its queued tail discarded. Implementations
     * with negligible buffering may return 0.
     */
    int getPendingPlaybackMs();
}
