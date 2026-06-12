package com.nerio.audioengine;

import java.io.File;
import java.io.IOException;

/**
 * A source of captured PCM audio. Extracted from {@link UsbAudioInput}'s
 * public surface so recording apps can swap capture sources — a USB ADC
 * ({@link UsbAudioInput}) or the phone's built-in microphone
 * ({@link AudioRecordInput}) — behind one type.
 *
 * <p>Lifecycle: {@link #configure(int, int, int)} then {@link #start()}
 * before {@link #startRecording(File)}; {@link #stopRecording()} then
 * {@link #stop()} to tear down; {@link #release()} frees the source.
 * After configure() the negotiated format may differ from the request —
 * read it back via the {@code getConfigured*()} getters.
 */
public interface AudioInput {

    /** True when this source can capture right now (device present, mic available). */
    boolean isAvailable();

    /** Sample rates this source supports. */
    int[] getSupportedRates();

    /** Bit depths this source supports. */
    int[] getSupportedBitDepths();

    /** Channel counts this source supports. */
    int[] getSupportedChannelCounts();

    boolean configure(int sampleRate, int channelCount, int requestedBitDepth);

    boolean start();

    /** Read raw PCM directly. Returns -1 if not capturing, 0 if no data ready. */
    int read(byte[] buf, int offset, int maxLen);

    /**
     * Starts a background thread that drains captured PCM into {@code wavOut}
     * as a canonical RIFF/WAVE file. Sizes are patched on {@link #stopRecording()}.
     */
    boolean startRecording(File wavOut) throws IOException;

    void stopRecording();

    void stop();

    void release();

    int getConfiguredRate();

    int getConfiguredChannels();

    int getConfiguredBitDepth();

    /** Bytes per sample as stored/transported; ≥ getConfiguredBitDepth()/8. */
    int getConfiguredSubslotSize();

    boolean isStarted();

    boolean isRecording();

    /** PCM frames written to the WAV so far. Zero until data actually flows. */
    long getFramesWritten();
}
