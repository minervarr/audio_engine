package com.nerio.audioengine;

import android.content.Context;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioRecord;
import android.media.MediaRecorder;
import android.media.audiofx.AcousticEchoCanceler;
import android.media.audiofx.AutomaticGainControl;
import android.media.audiofx.NoiseSuppressor;
import android.util.Log;

import java.io.File;
import java.io.IOException;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Built-in microphone capture via {@link AudioRecord}, used when no USB ADC
 * is connected. Mirrors {@link UsbAudioInput} behind {@link AudioInput} so a
 * recording app can swap sources without caring which is active.
 *
 * <p>Quality posture: prefers {@link MediaRecorder.AudioSource#UNPROCESSED}
 * (bypasses the platform's AGC/noise-suppression DSP) when the device
 * advertises support, falling back to {@code VOICE_RECOGNITION} — the
 * documented closest-to-raw source — otherwise. Any AGC/NS/AEC effects the
 * platform attaches to the session are defensively disabled. Bit depth 32
 * maps to {@link AudioFormat#ENCODING_PCM_FLOAT}; the float samples are
 * stored as an IEEE-float WAV (format tag 3).
 *
 * <p>The caller must hold the {@code RECORD_AUDIO} runtime permission before
 * {@link #configure(int, int, int)}; rate probing without it reports a
 * conservative default list.
 */
public class AudioRecordInput implements AudioInput {

    private static final String TAG = "AudioRecordInput";
    private static final int IDLE_POLL_SLEEP_MS = 2;
    private static final int[] PROBE_RATES = {192000, 96000, 48000, 44100};

    private final Context context;
    private final boolean unprocessedSupported;

    private LatencyProfile profile = LatencyProfile.STABLE;
    private AudioRecord audioRecord;
    private AutomaticGainControl agc;
    private NoiseSuppressor ns;
    private AcousticEchoCanceler aec;

    private volatile boolean started;
    private volatile boolean recording;
    private Thread recordThread;
    private WavFileWriter wavWriter;
    private final AtomicLong framesWritten = new AtomicLong(0);

    private int rate;
    private int channels;
    private int bitDepth;     // 16 or 32 (32 = IEEE float)
    private boolean floatEncoding;
    private int[] cachedRates;
    private float[] floatReadBuf;

    public AudioRecordInput(Context context) {
        this.context = context.getApplicationContext();
        AudioManager am = (AudioManager) this.context.getSystemService(Context.AUDIO_SERVICE);
        String prop = am != null
                ? am.getProperty(AudioManager.PROPERTY_SUPPORT_AUDIO_SOURCE_UNPROCESSED)
                : null;
        this.unprocessedSupported = "true".equals(prop);
        Log.i(TAG, "UNPROCESSED source supported: " + unprocessedSupported);
    }

    /** Latency profile must be set before configure(); affects buffer sizing. */
    public void setLatencyProfile(LatencyProfile profile) {
        if (profile != null) this.profile = profile;
    }

    /** True when capture runs on the UNPROCESSED source (no platform DSP). */
    public boolean isUnprocessed() {
        return unprocessedSupported;
    }

    private int audioSource() {
        return unprocessedSupported
                ? MediaRecorder.AudioSource.UNPROCESSED
                : MediaRecorder.AudioSource.VOICE_RECOGNITION;
    }

    @Override
    public boolean isAvailable() {
        return context.getPackageManager()
                .hasSystemFeature(android.content.pm.PackageManager.FEATURE_MICROPHONE);
    }

    @Override
    public int[] getSupportedRates() {
        if (cachedRates != null) return cachedRates;
        int count = 0;
        int[] found = new int[PROBE_RATES.length];
        for (int r : PROBE_RATES) {
            if (probeRate(r)) found[count++] = r;
        }
        if (count == 0) {
            // Probe failed entirely (likely missing RECORD_AUDIO) — report the
            // rates every Android device must support rather than nothing.
            cachedRates = new int[]{44100, 48000};
        } else {
            int[] out = new int[count];
            // Probe order is high→low; report ascending like UsbAudioInput.
            for (int i = 0; i < count; i++) out[i] = found[count - 1 - i];
            cachedRates = out;
        }
        return cachedRates;
    }

    private boolean probeRate(int sampleRate) {
        int minBuf = AudioRecord.getMinBufferSize(sampleRate,
                AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT);
        if (minBuf <= 0) return false;
        AudioRecord probe = null;
        try {
            // getMinBufferSize alone is unreliable on some OEMs; only a record
            // that actually reaches STATE_INITIALIZED proves the rate works.
            probe = new AudioRecord(audioSource(), sampleRate,
                    AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT, minBuf);
            return probe.getState() == AudioRecord.STATE_INITIALIZED;
        } catch (Exception e) {
            return false;
        } finally {
            if (probe != null) {
                try { probe.release(); } catch (Throwable ignored) {}
            }
        }
    }

    @Override
    public int[] getSupportedBitDepths() {
        return new int[]{16, 32};
    }

    @Override
    public int[] getSupportedChannelCounts() {
        return new int[]{1, 2};
    }

    @Override
    public boolean configure(int sampleRate, int channelCount, int requestedBitDepth) {
        releaseRecord();

        floatEncoding = requestedBitDepth >= 24;
        int encoding = floatEncoding
                ? AudioFormat.ENCODING_PCM_FLOAT : AudioFormat.ENCODING_PCM_16BIT;
        int channelMask = channelCount >= 2
                ? AudioFormat.CHANNEL_IN_STEREO : AudioFormat.CHANNEL_IN_MONO;

        int minBuf = AudioRecord.getMinBufferSize(sampleRate, channelMask, encoding);
        if (minBuf <= 0) {
            Log.e(TAG, "getMinBufferSize failed for " + sampleRate + "Hz");
            return false;
        }
        int bufSize = minBuf * (profile == LatencyProfile.STABLE ? 4 : 2);

        try {
            audioRecord = new AudioRecord.Builder()
                    .setAudioSource(audioSource())
                    .setAudioFormat(new AudioFormat.Builder()
                            .setSampleRate(sampleRate)
                            .setChannelMask(channelMask)
                            .setEncoding(encoding)
                            .build())
                    .setBufferSizeInBytes(bufSize)
                    .build();
        } catch (Exception e) {
            Log.e(TAG, "AudioRecord build failed (missing RECORD_AUDIO?)", e);
            return false;
        }
        if (audioRecord.getState() != AudioRecord.STATE_INITIALIZED) {
            Log.e(TAG, "AudioRecord did not initialize for " + sampleRate + "Hz");
            releaseRecord();
            return false;
        }

        // Even UNPROCESSED can have effects attached by some OEMs; force them off.
        disableSessionEffects(audioRecord.getAudioSessionId());

        this.rate = audioRecord.getSampleRate();
        this.channels = audioRecord.getChannelCount();
        this.bitDepth = floatEncoding ? 32 : 16;
        Log.i(TAG, "Configured mic capture: " + rate + "Hz/" + bitDepth
                + (floatEncoding ? "f" : "") + "bit/" + channels + "ch source="
                + (unprocessedSupported ? "UNPROCESSED" : "VOICE_RECOGNITION")
                + " buf=" + bufSize + " profile=" + profile);
        return true;
    }

    private void disableSessionEffects(int sessionId) {
        try {
            if (AutomaticGainControl.isAvailable()) {
                agc = AutomaticGainControl.create(sessionId);
                if (agc != null) agc.setEnabled(false);
            }
            if (NoiseSuppressor.isAvailable()) {
                ns = NoiseSuppressor.create(sessionId);
                if (ns != null) ns.setEnabled(false);
            }
            if (AcousticEchoCanceler.isAvailable()) {
                aec = AcousticEchoCanceler.create(sessionId);
                if (aec != null) aec.setEnabled(false);
            }
        } catch (Throwable t) {
            Log.w(TAG, "disabling session effects failed (non-fatal)", t);
        }
    }

    @Override
    public boolean start() {
        if (audioRecord == null) {
            Log.e(TAG, "start: not configured");
            return false;
        }
        try {
            audioRecord.startRecording();
        } catch (IllegalStateException e) {
            Log.e(TAG, "startRecording failed", e);
            return false;
        }
        started = audioRecord.getRecordingState() == AudioRecord.RECORDSTATE_RECORDING;
        if (!started) Log.e(TAG, "AudioRecord not in RECORDING state after start");
        return started;
    }

    @Override
    public int read(byte[] buf, int offset, int maxLen) {
        if (audioRecord == null || !started) return -1;
        if (floatEncoding) {
            int maxSamples = maxLen / 4;
            if (maxSamples <= 0) return 0;
            if (floatReadBuf == null || floatReadBuf.length < maxSamples) {
                floatReadBuf = new float[maxSamples];
            }
            int got = audioRecord.read(floatReadBuf, 0, maxSamples,
                    AudioRecord.READ_NON_BLOCKING);
            if (got <= 0) return got;
            packFloatsLE(floatReadBuf, got, buf, offset);
            return got * 4;
        }
        return audioRecord.read(buf, offset, maxLen, AudioRecord.READ_NON_BLOCKING);
    }

    @Override
    public boolean startRecording(File wavOut) throws IOException {
        if (!started) {
            Log.e(TAG, "startRecording: capture not started");
            return false;
        }
        if (recording) return false;
        if (wavOut == null) throw new IOException("wavOut == null");

        wavWriter = new WavFileWriter(wavOut, rate, channels, bitDepth, floatEncoding);
        framesWritten.set(0);

        recording = true;
        recordThread = new Thread(this::recordLoop, "AudioRecordInput-WAV");
        recordThread.start();
        Log.i(TAG, "startRecording -> " + wavOut.getAbsolutePath());
        return true;
    }

    @Override
    public void stopRecording() {
        recording = false;
        Thread t = recordThread;
        recordThread = null;
        if (t != null) {
            try {
                t.join(2000);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
        }
        WavFileWriter w = wavWriter;
        wavWriter = null;
        if (w != null) {
            try {
                long bytes = w.getPcmBytes();
                w.close();
                Log.i(TAG, "stopRecording: wrote " + bytes + " PCM bytes");
            } catch (IOException e) {
                Log.e(TAG, "wav close failed", e);
            }
        }
    }

    private void recordLoop() {
        // Non-blocking reads with a short idle sleep, mirroring UsbAudioInput's
        // ring-drain loop, so stopRecording()'s bounded join can't hang on a
        // blocking AudioRecord.read.
        final int frameBytes = ((bitDepth + 7) / 8) * channels;
        final byte[] byteBuf = new byte[16 * 1024];
        while (true) {
            int got = read(byteBuf, 0, byteBuf.length);
            if (got < 0) {
                Log.w(TAG, "read returned " + got + " -- ending record loop");
                break;
            }
            if (got == 0) {
                if (!recording) break;
                try {
                    Thread.sleep(IDLE_POLL_SLEEP_MS);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    break;
                }
                continue;
            }
            try {
                wavWriter.write(byteBuf, 0, got);
                if (frameBytes > 0) {
                    framesWritten.addAndGet(got / frameBytes);
                }
            } catch (IOException e) {
                Log.e(TAG, "wav write failed", e);
                break;
            }
        }
    }

    /** Pack float samples as 32-bit IEEE little-endian, matching WAV format tag 3. */
    private static void packFloatsLE(float[] src, int count, byte[] dst, int dstOffset) {
        int o = dstOffset;
        for (int i = 0; i < count; i++) {
            int bits = Float.floatToRawIntBits(src[i]);
            dst[o++] = (byte) (bits & 0xFF);
            dst[o++] = (byte) ((bits >>> 8) & 0xFF);
            dst[o++] = (byte) ((bits >>> 16) & 0xFF);
            dst[o++] = (byte) ((bits >>> 24) & 0xFF);
        }
    }

    @Override
    public void stop() {
        stopRecording();
        if (audioRecord != null && started) {
            try {
                audioRecord.stop();
            } catch (IllegalStateException ignored) {}
            started = false;
        }
    }

    @Override
    public void release() {
        stop();
        releaseRecord();
        cachedRates = null;
    }

    private void releaseRecord() {
        if (agc != null) { try { agc.release(); } catch (Throwable ignored) {} agc = null; }
        if (ns != null) { try { ns.release(); } catch (Throwable ignored) {} ns = null; }
        if (aec != null) { try { aec.release(); } catch (Throwable ignored) {} aec = null; }
        if (audioRecord != null) {
            try { audioRecord.release(); } catch (Throwable ignored) {}
            audioRecord = null;
        }
        started = false;
    }

    @Override
    public int getConfiguredRate() { return rate; }

    @Override
    public int getConfiguredChannels() { return channels; }

    @Override
    public int getConfiguredBitDepth() { return bitDepth; }

    @Override
    public int getConfiguredSubslotSize() { return (bitDepth + 7) / 8; }

    @Override
    public boolean isStarted() { return started; }

    @Override
    public boolean isRecording() { return recording; }

    @Override
    public long getFramesWritten() { return framesWritten.get(); }
}
