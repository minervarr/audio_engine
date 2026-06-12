package com.nerio.audioengine;

/**
 * Latency/stability trade-off for the audio pipelines.
 *
 * <p>{@link #STABLE} (the default) uses deep USB transfer queues and large
 * ring buffers so scheduling stalls can't cause dropouts — right for
 * recording, where a single drop ruins a take. {@link #LOW_LATENCY} uses
 * shallow queues for live monitoring at the cost of dropout resistance.
 *
 * <p>Applies to {@link UsbAudioDevice#setLatencyProfile}, the
 * {@link AudioTrackOutput} performance mode/buffer sizing, and
 * {@link AudioRecordInput} buffer sizing. Must be set before
 * configure()/start(); changes are ignored mid-stream.
 */
public enum LatencyProfile {
    LOW_LATENCY(0),
    STABLE(1);

    private final int nativeValue;

    LatencyProfile(int nativeValue) {
        this.nativeValue = nativeValue;
    }

    public int nativeValue() {
        return nativeValue;
    }
}
