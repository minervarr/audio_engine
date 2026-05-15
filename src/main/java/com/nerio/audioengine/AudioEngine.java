package com.nerio.audioengine;

import android.content.Context;
import android.media.AudioFormat;
import android.media.MediaCodec;
import android.media.MediaExtractor;
import android.media.MediaFormat;
import android.net.Uri;
import android.util.Log;

import android.os.Process;

import android.media.MediaDataSource;

import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public class AudioEngine {

    private static final String TAG = "AudioEngine";
    private static final int PCM_BUFFER_SIZE = 1024 * 1024; // 1MB ring buffer

    private static final byte[] BIT_REVERSE = new byte[256];
    static {
        for (int i = 0; i < 256; i++) {
            int r = 0;
            for (int b = 0; b < 8; b++) {
                if ((i & (1 << b)) != 0) r |= 1 << (7 - b);
            }
            BIT_REVERSE[i] = (byte) r;
        }
    }

    public interface OnPreparedListener {
        void onPrepared(AudioEngine engine);
    }

    public interface OnCompletionListener {
        void onCompletion(AudioEngine engine);
    }

    public interface OnErrorListener {
        void onError(AudioEngine engine, String message);
    }

    public interface OnTransitionListener {
        void onTransition(AudioEngine engine);
    }

    private OnPreparedListener onPreparedListener;
    private OnCompletionListener onCompletionListener;
    private OnErrorListener onErrorListener;

    private MediaExtractor extractor;
    private MediaCodec codec;
    private AudioOutput output;
    private NativePcmBuffer pcmBuffer;
    private NativePcmBuffer nextPcmBufferPool; // reusable buffer for gapless pre-decode
    private MediaDataSource streamDataSource;
    private ExecutorService seekExecutor = Executors.newSingleThreadExecutor();
    private final Object outputLock = new Object();
    private final Object pauseLock = new Object();
    private final Object codecLock = new Object();

    private Thread decodeThread;
    private Thread outputThread;

    private volatile boolean playing;
    private volatile boolean stopped;
    private volatile boolean released;
    private volatile boolean seeking;
    private volatile boolean prepared;
    private volatile boolean inputDone;
    private volatile boolean outputFailed;

    // Gapless: encoder delay/padding for current track
    private int encoderDelay;
    private int encoderPadding;

    // Gapless: next track pre-decode pipeline
    private MediaExtractor nextExtractor;
    private MediaCodec nextCodec;
    private NativePcmBuffer nextPcmBuffer;
    private Thread nextDecodeThread;
    private MediaDataSource nextStreamDataSource;
    private volatile boolean nextStopped = true;
    private long nextDurationUs;
    private int nextSampleRate, nextChannelCount, nextEncoding, nextSourceBitDepth;
    private String nextMime, nextCodecName;
    private int nextEncoderDelay;
    private int nextEncoderPadding;

    private OnTransitionListener onTransitionListener;

    private EqProcessor eqProcessor;

    private int sampleRate;
    private int channelCount;
    private int encoding;
    private int sourceBitDepth;
    private long durationUs;

    private String mime;
    private String codecName;

    private int cachedFrameSize;

    private boolean isDsd;
    private int dsdRate;
    private DsfParser dsfParser;
    private DffParser dffParser;
    private RandomAccessFile dsdFile;

    private volatile long currentPositionUs;
    private volatile long seekTargetUs = -1;
    private volatile long playbackBaseUs;
    private volatile long bytesWritten;

    public void setOnPreparedListener(OnPreparedListener listener) {
        this.onPreparedListener = listener;
    }

    public void setOnCompletionListener(OnCompletionListener listener) {
        this.onCompletionListener = listener;
    }

    public void setOnErrorListener(OnErrorListener listener) {
        this.onErrorListener = listener;
    }

    public void setOnTransitionListener(OnTransitionListener listener) {
        this.onTransitionListener = listener;
    }

    public void setEqProcessor(EqProcessor proc) {
        this.eqProcessor = proc;
    }

    public EqProcessor getEqProcessor() {
        return eqProcessor;
    }

    public int getSampleRate() {
        return sampleRate;
    }

    public int getChannelCount() {
        return channelCount;
    }

    public int getEncoding() {
        return encoding;
    }

    public int getSourceBitDepth() {
        return sourceBitDepth;
    }

    public boolean isDsd() {
        return isDsd;
    }

    public boolean isDopMode() {
        return false;
    }

    public int getDsdRate() {
        return dsdRate;
    }

    public String getMime() {
        return mime;
    }

    public String getCodecName() {
        return codecName;
    }

    public AudioOutput getOutput() {
        synchronized (outputLock) {
            return output;
        }
    }

    public void play(Context context, Uri uri) {
        Log.d(TAG, "play: " + uri);
        stop();

        if (pcmBuffer == null) {
            pcmBuffer = new NativePcmBuffer(PCM_BUFFER_SIZE);
        } else {
            pcmBuffer.reset();
        }
        stopped = false;
        seeking = false;
        prepared = false;
        outputFailed = false;
        currentPositionUs = 0;
        seekTargetUs = -1;
        playbackBaseUs = 0;
        bytesWritten = 0;
        isDsd = false;
        dsdRate = 0;
        encoderDelay = 0;
        encoderPadding = 0;

        // Detect DSD by file extension
        String path = uri.getPath();
        if (path != null) {
            String lower = path.toLowerCase(Locale.ROOT);
            if (lower.endsWith(".dsf") || lower.endsWith(".dff")) {
                playDsd(path, lower.endsWith(".dsf"));
                return;
            }
        }

        try {
            extractor = new MediaExtractor();
            extractor.setDataSource(context, uri, null);

            int audioTrackIndex = -1;
            mime = null;
            MediaFormat format = null;

            for (int i = 0; i < extractor.getTrackCount(); i++) {
                format = extractor.getTrackFormat(i);
                mime = format.getString(MediaFormat.KEY_MIME);
                if (mime != null && mime.startsWith("audio/")) {
                    audioTrackIndex = i;
                    break;
                }
            }

            if (audioTrackIndex < 0) {
                notifyError("No audio track found");
                return;
            }

            extractor.selectTrack(audioTrackIndex);

            sampleRate = format.getInteger(MediaFormat.KEY_SAMPLE_RATE);
            channelCount = format.getInteger(MediaFormat.KEY_CHANNEL_COUNT);
            durationUs = format.containsKey(MediaFormat.KEY_DURATION)
                    ? format.getLong(MediaFormat.KEY_DURATION) : 0;

            // Detect source bit depth from extractor format (before codec decoding)
            sourceBitDepth = 0;
            if (format.containsKey("bits-per-sample")) {
                sourceBitDepth = format.getInteger("bits-per-sample");
            } else if (format.containsKey("bit-width")) {
                sourceBitDepth = format.getInteger("bit-width");
            } else if (format.containsKey(MediaFormat.KEY_PCM_ENCODING)) {
                int srcEnc = format.getInteger(MediaFormat.KEY_PCM_ENCODING);
                switch (srcEnc) {
                    case AudioFormat.ENCODING_PCM_FLOAT:
                    case AudioFormat.ENCODING_PCM_32BIT:
                        sourceBitDepth = 32;
                        break;
                    case AudioFormat.ENCODING_PCM_24BIT_PACKED:
                        sourceBitDepth = 24;
                        break;
                    case AudioFormat.ENCODING_PCM_16BIT:
                        sourceBitDepth = 16;
                        break;
                }
            }
            if (sourceBitDepth <= 0) {
                sourceBitDepth = 16;
            }
            Log.d(TAG, "Source bit depth detected: " + sourceBitDepth);

            if (isLosslessMime(mime) && sourceBitDepth > 16) {
                format.setInteger(MediaFormat.KEY_PCM_ENCODING, AudioFormat.ENCODING_PCM_FLOAT);
            }

            codec = MediaCodec.createDecoderByType(mime);
            codecName = codec.getName();
            codec.configure(format, null, null, 0);
            codec.start();

            // Read gapless metadata (encoder delay/padding for MP3/AAC)
            if (format.containsKey("encoder-delay")) {
                encoderDelay = format.getInteger("encoder-delay");
            }
            if (format.containsKey("encoder-padding")) {
                encoderPadding = format.getInteger("encoder-padding");
            }
            if (encoderDelay > 0 || encoderPadding > 0) {
                Log.d(TAG, "Gapless metadata: delay=" + encoderDelay + " padding=" + encoderPadding);
            }

            // Detect actual output encoding from codec
            MediaFormat outputFormat = codec.getOutputFormat();
            if (outputFormat.containsKey(MediaFormat.KEY_PCM_ENCODING)) {
                encoding = outputFormat.getInteger(MediaFormat.KEY_PCM_ENCODING);
            } else {
                encoding = AudioFormat.ENCODING_PCM_16BIT;
            }

            String encName;
            switch (encoding) {
                case AudioFormat.ENCODING_PCM_FLOAT: encName = "PCM_FLOAT"; break;
                case AudioFormat.ENCODING_PCM_24BIT_PACKED: encName = "PCM_24BIT"; break;
                case AudioFormat.ENCODING_PCM_32BIT: encName = "PCM_32BIT"; break;
                default: encName = "PCM_16BIT"; break;
            }
            Log.d(TAG, "play: configured " + sampleRate + "Hz/" + channelCount + "ch/" + encName + " sourceBitDepth=" + sourceBitDepth + " duration=" + (durationUs / 1000) + "ms");

            synchronized (outputLock) {
                if (output == null) {
                    output = new AudioTrackOutput();
                }
                output.configure(sampleRate, channelCount, encoding, sourceBitDepth);
                output.start();
            }

            playing = true;
            prepared = true;
            cachedFrameSize = channelCount * bytesPerSample();

            startDecodeThread();
            startOutputThread();

            if (onPreparedListener != null) {
                onPreparedListener.onPrepared(this);
            }

        } catch (Exception e) {
            Log.e(TAG, "Error setting up playback", e);
            notifyError("Could not play track");
        }
    }

    public void playStream(MediaDataSource dataSource, long durationHintUs) {
        Log.d(TAG, "playStream: durationHint=" + (durationHintUs / 1000) + "ms");
        stop();

        if (seekExecutor == null || seekExecutor.isShutdown()) {
            seekExecutor = Executors.newSingleThreadExecutor();
        }
        if (pcmBuffer == null) {
            pcmBuffer = new NativePcmBuffer(PCM_BUFFER_SIZE);
        } else {
            pcmBuffer.reset();
        }
        stopped = false;
        seeking = false;
        prepared = false;
        outputFailed = false;
        currentPositionUs = 0;
        seekTargetUs = -1;
        playbackBaseUs = 0;
        bytesWritten = 0;
        isDsd = false;
        dsdRate = 0;
        encoderDelay = 0;
        encoderPadding = 0;

        try {
            this.streamDataSource = dataSource;

            // Use local variables for setup to avoid NPE if stop() is called
            // concurrently during blocking network I/O (setDataSource, etc.)
            MediaExtractor localExtractor = new MediaExtractor();
            localExtractor.setDataSource(dataSource);

            if (stopped) {
                localExtractor.release();
                return;
            }

            int audioTrackIndex = -1;
            mime = null;
            MediaFormat format = null;

            for (int i = 0; i < localExtractor.getTrackCount(); i++) {
                format = localExtractor.getTrackFormat(i);
                mime = format.getString(MediaFormat.KEY_MIME);
                if (mime != null && mime.startsWith("audio/")) {
                    audioTrackIndex = i;
                    break;
                }
            }

            if (audioTrackIndex < 0) {
                localExtractor.release();
                notifyError("No audio track found");
                return;
            }

            localExtractor.selectTrack(audioTrackIndex);

            sampleRate = format.getInteger(MediaFormat.KEY_SAMPLE_RATE);
            channelCount = format.getInteger(MediaFormat.KEY_CHANNEL_COUNT);
            durationUs = format.containsKey(MediaFormat.KEY_DURATION)
                    ? format.getLong(MediaFormat.KEY_DURATION) : 0;
            if (durationUs <= 0 && durationHintUs > 0) {
                durationUs = durationHintUs;
            }

            sourceBitDepth = 0;
            if (format.containsKey("bits-per-sample")) {
                sourceBitDepth = format.getInteger("bits-per-sample");
            } else if (format.containsKey("bit-width")) {
                sourceBitDepth = format.getInteger("bit-width");
            } else if (format.containsKey(MediaFormat.KEY_PCM_ENCODING)) {
                int srcEnc = format.getInteger(MediaFormat.KEY_PCM_ENCODING);
                switch (srcEnc) {
                    case AudioFormat.ENCODING_PCM_FLOAT:
                    case AudioFormat.ENCODING_PCM_32BIT:
                        sourceBitDepth = 32;
                        break;
                    case AudioFormat.ENCODING_PCM_24BIT_PACKED:
                        sourceBitDepth = 24;
                        break;
                    case AudioFormat.ENCODING_PCM_16BIT:
                        sourceBitDepth = 16;
                        break;
                }
            }
            if (sourceBitDepth <= 0) {
                sourceBitDepth = 16;
            }
            Log.d(TAG, "Source bit depth detected: " + sourceBitDepth);

            if (stopped) {
                localExtractor.release();
                return;
            }

            if (isLosslessMime(mime) && sourceBitDepth > 16) {
                format.setInteger(MediaFormat.KEY_PCM_ENCODING, AudioFormat.ENCODING_PCM_FLOAT);
            }

            MediaCodec localCodec = MediaCodec.createDecoderByType(mime);
            codecName = localCodec.getName();
            localCodec.configure(format, null, null, 0);
            localCodec.start();

            // Read gapless metadata (encoder delay/padding for MP3/AAC)
            if (format.containsKey("encoder-delay")) {
                encoderDelay = format.getInteger("encoder-delay");
            }
            if (format.containsKey("encoder-padding")) {
                encoderPadding = format.getInteger("encoder-padding");
            }
            if (encoderDelay > 0 || encoderPadding > 0) {
                Log.d(TAG, "Gapless metadata: delay=" + encoderDelay + " padding=" + encoderPadding);
            }

            if (stopped) {
                localCodec.stop();
                localCodec.release();
                localExtractor.release();
                return;
            }

            MediaFormat outputFormat = localCodec.getOutputFormat();
            if (outputFormat.containsKey(MediaFormat.KEY_PCM_ENCODING)) {
                encoding = outputFormat.getInteger(MediaFormat.KEY_PCM_ENCODING);
            } else {
                encoding = AudioFormat.ENCODING_PCM_16BIT;
            }

            String encName;
            switch (encoding) {
                case AudioFormat.ENCODING_PCM_FLOAT: encName = "PCM_FLOAT"; break;
                case AudioFormat.ENCODING_PCM_24BIT_PACKED: encName = "PCM_24BIT"; break;
                case AudioFormat.ENCODING_PCM_32BIT: encName = "PCM_32BIT"; break;
                default: encName = "PCM_16BIT"; break;
            }
            Log.d(TAG, "playStream: configured " + sampleRate + "Hz/" + channelCount + "ch/" + encName + " sourceBitDepth=" + sourceBitDepth + " duration=" + (durationUs / 1000) + "ms");

            // Assign locals to fields inside codecLock with final stopped check
            synchronized (codecLock) {
                if (stopped) {
                    localCodec.stop();
                    localCodec.release();
                    localExtractor.release();
                    return;
                }
                extractor = localExtractor;
                codec = localCodec;
            }

            synchronized (outputLock) {
                if (output == null) {
                    output = new AudioTrackOutput();
                }
                output.configure(sampleRate, channelCount, encoding, sourceBitDepth);
                output.start();
            }

            playing = true;
            prepared = true;
            cachedFrameSize = channelCount * bytesPerSample();

            startDecodeThread();
            startOutputThread();

            if (onPreparedListener != null) {
                onPreparedListener.onPrepared(this);
            }

        } catch (Exception e) {
            Log.e(TAG, "Error setting up stream playback", e);
            if (!stopped) {
                notifyError("Could not play stream");
            }
        }
    }

    /**
     * Switch to a new audio output. Returns true on success.
     * On failure, automatically falls back to AudioTrackOutput.
     */
    public boolean switchOutput(AudioOutput newOutput) {
        Log.d(TAG, "switchOutput: " + newOutput.getClass().getSimpleName());
        synchronized (outputLock) {
            if (isDsd && newOutput instanceof AudioTrackOutput) {
                Log.e(TAG, "Cannot switch DSD to speaker output");
                newOutput.release();
                return false;
            }

            outputFailed = false;
            boolean wasPlaying = playing;
            if (wasPlaying && output != null) {
                output.pause();
            }
            if (output != null) {
                output.flush();
                output.release();
            }
            output = newOutput;
            if (prepared) {
                if (!output.configure(sampleRate, channelCount, encoding, sourceBitDepth)) {
                    Log.e(TAG, "New output configure failed");
                    output.release();
                    if (isDsd) {
                        output = null;
                        return false;
                    }
                    output = new AudioTrackOutput();
                    output.configure(sampleRate, channelCount, encoding, sourceBitDepth);
                    output.start();
                    if (!wasPlaying) output.pause();
                    return false;
                }
                if (!output.start()) {
                    Log.e(TAG, "New output start failed");
                    output.release();
                    if (isDsd) {
                        output = null;
                        return false;
                    }
                    output = new AudioTrackOutput();
                    output.configure(sampleRate, channelCount, encoding, sourceBitDepth);
                    output.start();
                    if (!wasPlaying) output.pause();
                    return false;
                }
                if (!wasPlaying) {
                    output.pause();
                }
            }
            Log.d(TAG, "switchOutput: success");
            outputLock.notifyAll();
            return true;
        }
    }

    private void playDsd(String path, boolean isDsf) {
        try {
            dsdFile = new RandomAccessFile(path, "r");

            if (isDsf) {
                dsfParser = new DsfParser();
                dsfParser.parse(dsdFile);
                dsdRate = dsfParser.getSampleRate();
                channelCount = dsfParser.getChannelCount();
                long totalSamples = dsfParser.getTotalSamples();
                durationUs = totalSamples * 1_000_000L / dsdRate;
                Log.d(TAG, "DSD detected: DSF, rate=" + dsdRate
                        + ", channels=" + channelCount
                        + ", bitsPerSample=" + dsfParser.getBitsPerSample()
                        + (dsfParser.getBitsPerSample() == 1 ? " (LSB)" : " (MSB)"));
            } else {
                dffParser = new DffParser();
                dffParser.parse(dsdFile);
                dsdRate = dffParser.getSampleRate();
                channelCount = dffParser.getChannelCount();
                long totalSamples = dffParser.getTotalSamples();
                durationUs = totalSamples * 1_000_000L / dsdRate;
                Log.d(TAG, "DSD detected: DFF, rate=" + dsdRate
                        + ", channels=" + channelCount);
            }

            isDsd = true;
            sampleRate = dsdRate / 32;
            encoding = AudioFormat.ENCODING_PCM_32BIT;
            sourceBitDepth = 1;
            mime = "audio/dsd";
            codecName = "DSD Native";

            Log.d(TAG, "DSD play: pcmRate=" + sampleRate + " dsdRate=" + dsdRate
                    + " duration=" + (durationUs / 1000) + "ms");

            synchronized (outputLock) {
                if (output == null) {
                    output = new AudioTrackOutput();
                }
                if (!output.configure(sampleRate, channelCount, encoding, sourceBitDepth)) {
                    // Speaker cannot handle DSD -- release output, await USB switch
                    Log.w(TAG, "Output cannot handle DSD, awaiting USB switch");
                    output.release();
                    output = null;
                } else {
                    output.start();
                }
            }

            playing = true;
            prepared = true;
            cachedFrameSize = channelCount * bytesPerSample();

            startDsdDecodeThread();
            startOutputThread();

            if (onPreparedListener != null) {
                onPreparedListener.onPrepared(this);
            }

        } catch (Exception e) {
            Log.e(TAG, "Error setting up DSD playback", e);
            closeDsdResources();
            notifyError("Could not play DSD track");
        }
    }

    private void startDsdDecodeThread() {
        Log.d(TAG, "DSD decode thread starting");
        decodeThread = new Thread(() -> {
            int blockSize;
            boolean needBitReverse;

            if (dsfParser != null) {
                blockSize = dsfParser.getBlockSizePerChannel();
                needBitReverse = dsfParser.getBitsPerSample() == 1;
            } else {
                blockSize = dffParser.getBlockSizePerChannel();
                needBitReverse = false;
            }

            byte[] leftBlock = new byte[blockSize];
            byte[] rightBlock = new byte[blockSize];

            int framesPerBlock = blockSize / 4;
            byte[] packed = new byte[framesPerBlock * channelCount * 4];

            while (!stopped) {
                if (seeking) {
                    try { Thread.sleep(5); } catch (InterruptedException e) { break; }
                    continue;
                }

                synchronized (pauseLock) {
                    while (!playing && !stopped) {
                        try { pauseLock.wait(); } catch (InterruptedException e) { break; }
                    }
                }
                if (stopped) break;

                try {
                    boolean hasData;
                    if (dsfParser != null) {
                        hasData = dsfParser.readBlockPair(leftBlock, rightBlock);
                    } else {
                        hasData = dffParser.readBlockPair(leftBlock, rightBlock);
                    }

                    if (!hasData) {
                        pcmBuffer.signalEnd();
                        break;
                    }

                    if (needBitReverse) {
                        for (int i = 0; i < blockSize; i++) {
                            leftBlock[i] = BIT_REVERSE[leftBlock[i] & 0xFF];
                            rightBlock[i] = BIT_REVERSE[rightBlock[i] & 0xFF];
                        }
                    }

                    // Pack into 32-bit interleaved frames: [4 L bytes][4 R bytes]
                    int packedPos = 0;
                    if (channelCount >= 2) {
                        for (int i = 0; i < blockSize; i += 4) {
                            System.arraycopy(leftBlock, i, packed, packedPos, 4);
                            packedPos += 4;
                            System.arraycopy(rightBlock, i, packed, packedPos, 4);
                            packedPos += 4;
                        }
                    } else {
                        for (int i = 0; i < blockSize; i += 4) {
                            System.arraycopy(leftBlock, i, packed, packedPos, 4);
                            packedPos += 4;
                        }
                    }

                    pcmBuffer.write(packed, 0, packedPos);

                } catch (InterruptedException e) {
                    break;
                } catch (IOException e) {
                    Log.e(TAG, "DSD read error", e);
                    pcmBuffer.signalEnd();
                    break;
                }
            }
        }, "AudioEngine-DSD-Decode");
        decodeThread.start();
    }

    private void seekDsd(int positionMs) {
        seeking = true;
        long targetUs = (long) positionMs * 1000;

        pcmBuffer.flush();
        synchronized (outputLock) {
            if (output != null) {
                output.pause();
                output.flush();
                output.resume();
            }
        }

        int blockSize;
        if (dsfParser != null) {
            blockSize = dsfParser.getBlockSizePerChannel();
        } else {
            blockSize = dffParser.getBlockSizePerChannel();
        }

        long targetSample = targetUs * dsdRate / 1_000_000L;
        long samplesPerBlock = (long) blockSize * 8;
        int targetBlock = (int) (targetSample / samplesPerBlock);

        if (dsfParser != null) {
            dsfParser.seekToBlock(targetBlock);
        } else {
            dffParser.seekToBlock(targetBlock);
        }

        playbackBaseUs = targetUs;
        bytesWritten = 0;
        currentPositionUs = targetUs;

        seeking = false;
        synchronized (pauseLock) {
            pauseLock.notifyAll();
        }
    }

    private void closeDsdResources() {
        dsfParser = null;
        dffParser = null;
        isDsd = false;
        dsdRate = 0;
        if (dsdFile != null) {
            try { dsdFile.close(); } catch (Exception ignored) {}
            dsdFile = null;
        }
    }

    private int bytesPerSample() {
        switch (encoding) {
            case AudioFormat.ENCODING_PCM_FLOAT: return 4;
            case AudioFormat.ENCODING_PCM_32BIT: return 4;
            case AudioFormat.ENCODING_PCM_24BIT_PACKED: return 3;
            default: return 2;
        }
    }

    private void startDecodeThread() {
        Log.d(TAG, "decode thread starting");
        decodeThread = new Thread(() -> {
            MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
            inputDone = false;
            boolean outputDone = false;
            byte[] decodeBuf = null;

            // Gapless: native encoder delay/padding handling
            int frameSize = channelCount * bytesPerSample();
            NativeGaplessDecoder gaplessDecoder = new NativeGaplessDecoder(
                    encoderDelay, encoderPadding, frameSize, pcmBuffer);
            boolean wasSeekingLastLoop = false;

            while (!stopped && !outputDone) {
                if (seeking) {
                    wasSeekingLastLoop = true;
                    try { Thread.sleep(5); } catch (InterruptedException e) { break; }
                    continue;
                }
                if (wasSeekingLastLoop) {
                    wasSeekingLastLoop = false;
                    gaplessDecoder.resetAfterSeek();
                }

                synchronized (pauseLock) {
                    while (!playing && !stopped) {
                        try { pauseLock.wait(); } catch (InterruptedException e) { break; }
                    }
                }
                if (stopped) break;

                int decodeSize = 0;
                boolean endOfStream = false;

                synchronized (codecLock) {
                    if (seeking || stopped) continue;

                    if (!inputDone) {
                        int inputIndex = codec.dequeueInputBuffer(10000);
                        if (inputIndex >= 0) {
                            ByteBuffer inputBuf = codec.getInputBuffer(inputIndex);
                            int sampleSize = extractor.readSampleData(inputBuf, 0);
                            if (sampleSize < 0) {
                                codec.queueInputBuffer(inputIndex, 0, 0, 0,
                                        MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                                inputDone = true;
                            } else {
                                long pts = extractor.getSampleTime();
                                codec.queueInputBuffer(inputIndex, 0, sampleSize, pts, 0);
                                extractor.advance();
                            }
                        }
                    }

                    int outputIndex = codec.dequeueOutputBuffer(info, 10000);
                    if (outputIndex >= 0) {
                        if ((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) {
                            endOfStream = true;
                            codec.releaseOutputBuffer(outputIndex, false);
                        } else if (info.size > 0) {
                            ByteBuffer outputBuf = codec.getOutputBuffer(outputIndex);
                            if (decodeBuf == null || decodeBuf.length < info.size) {
                                decodeBuf = new byte[info.size];
                            }
                            outputBuf.get(decodeBuf, 0, info.size);
                            decodeSize = info.size;
                            codec.releaseOutputBuffer(outputIndex, false);
                        } else {
                            codec.releaseOutputBuffer(outputIndex, false);
                        }
                    } else if (outputIndex == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                        MediaFormat newFormat = codec.getOutputFormat();
                        int newRate = newFormat.getInteger(MediaFormat.KEY_SAMPLE_RATE);
                        int newChannels = newFormat.getInteger(MediaFormat.KEY_CHANNEL_COUNT);
                        int newEncoding = encoding;
                        if (newFormat.containsKey(MediaFormat.KEY_PCM_ENCODING)) {
                            newEncoding = newFormat.getInteger(MediaFormat.KEY_PCM_ENCODING);
                        }
                        if (newRate != sampleRate || newChannels != channelCount || newEncoding != encoding) {
                            sampleRate = newRate;
                            channelCount = newChannels;
                            encoding = newEncoding;
                        }
                    }
                }

                if (endOfStream && !seeking) {
                    outputDone = true;
                    gaplessDecoder.signalEnd();
                } else if (decodeSize > 0) {
                    try {
                        gaplessDecoder.processFrame(decodeBuf, 0, decodeSize);
                    } catch (InterruptedException e) {
                        break;
                    }
                }
            }
            gaplessDecoder.destroy();
        }, "AudioEngine-Decode");
        decodeThread.start();
    }

    private void startOutputThread() {
        Log.d(TAG, "output thread starting");
        outputThread = new Thread(() -> {
            Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO);
            byte[] readBuf = new byte[16384];

            while (!stopped) {
                if (seeking) {
                    try { Thread.sleep(5); } catch (InterruptedException e) { break; }
                    continue;
                }

                synchronized (pauseLock) {
                    while (!playing && !stopped) {
                        try { pauseLock.wait(); } catch (InterruptedException e) { break; }
                    }
                }
                if (stopped) break;

                try {
                    int read = pcmBuffer.read(readBuf, 0, readBuf.length);
                    if (read == -1) {
                        // Gapless: check for pre-decoded next track
                        if (!stopped && !seeking && nextPcmBuffer != null && !nextStopped) {
                            boolean formatMatch = (nextSampleRate == sampleRate
                                    && nextChannelCount == channelCount
                                    && nextEncoding == encoding);

                            // Release old decoder, promote next
                            synchronized (codecLock) {
                                MediaExtractor oldExt = extractor;
                                MediaCodec oldCod = codec;
                                extractor = nextExtractor;
                                codec = nextCodec;
                                nextExtractor = null;
                                nextCodec = null;
                                if (oldCod != null) {
                                    try { oldCod.stop(); oldCod.release(); } catch (Exception ignored) {}
                                }
                                if (oldExt != null) {
                                    oldExt.release();
                                }
                            }

                            // Close old stream data source to release temp file
                            MediaDataSource oldSource = streamDataSource;

                            // Swap buffers
                            pcmBuffer = nextPcmBuffer;
                            nextPcmBuffer = null;
                            streamDataSource = nextStreamDataSource;
                            nextStreamDataSource = null;

                            // Clean up old source (after extractor is released above)
                            if (oldSource != null) {
                                try { oldSource.close(); } catch (Exception ignored) {}
                            }

                            // Update format/position tracking
                            sampleRate = nextSampleRate;
                            channelCount = nextChannelCount;
                            encoding = nextEncoding;
                            cachedFrameSize = channelCount * bytesPerSample();
                            sourceBitDepth = nextSourceBitDepth;
                            durationUs = nextDurationUs;
                            mime = nextMime;
                            codecName = nextCodecName;
                            encoderDelay = nextEncoderDelay;
                            encoderPadding = nextEncoderPadding;
                            playbackBaseUs = 0;
                            bytesWritten = 0;
                            currentPositionUs = 0;

                            if (!formatMatch) {
                                synchronized (outputLock) {
                                    if (output != null) {
                                        output.flush();
                                        output.configure(sampleRate, channelCount, encoding, sourceBitDepth);
                                        output.start();
                                    }
                                }
                                // Reconfigure EQ for new sample rate
                                if (eqProcessor != null && eqProcessor.getCurrentProfile() != null) {
                                    eqProcessor.computeCoefficients(
                                            eqProcessor.getCurrentProfile(),
                                            sampleRate, channelCount, encoding);
                                }
                            }

                            // Promote decode thread
                            decodeThread = nextDecodeThread;
                            nextDecodeThread = null;

                            Log.d(TAG, "Gapless transition: " + sampleRate + "Hz/" + channelCount + "ch"
                                    + (formatMatch ? " (seamless)" : " (reconfigured)"));

                            if (onTransitionListener != null) {
                                onTransitionListener.onTransition(AudioEngine.this);
                            }
                            continue;
                        }

                        if (!stopped && !seeking && onCompletionListener != null) {
                            onCompletionListener.onCompletion(this);
                        }
                        if (seeking) continue;
                        break;
                    } else if (read == -2) {
                        continue;
                    } else if (read > 0) {
                        if (eqProcessor != null && !isDsd) {
                            eqProcessor.process(readBuf, 0, read);
                        }
                        int pos = 0;
                        while (pos < read && !stopped && !seeking && playing) {
                            int w;
                            synchronized (outputLock) {
                                if (output != null) {
                                    w = output.write(readBuf, pos, read - pos);
                                } else {
                                    try { outputLock.wait(50); } catch (InterruptedException e) { break; }
                                    continue;
                                }
                            }
                            if (w > 0) {
                                pos += w;
                                if (outputFailed) outputFailed = false;
                            } else if (w == 0) {
                                try { Thread.sleep(1); } catch (InterruptedException e) { break; }
                            } else {
                                // w < 0: output died (e.g. USB transfers failed)
                                if (!outputFailed) {
                                    outputFailed = true;
                                    Log.e(TAG, "Output write failed (returned " + w + ")");
                                    if (onErrorListener != null) {
                                        onErrorListener.onError(this, "OUTPUT_FAILED");
                                    }
                                }
                                // Sleep and retry -- the main thread may switch outputs
                                try { Thread.sleep(10); } catch (InterruptedException e) { break; }
                            }
                        }
                        if (pos > 0 && !seeking) {
                            bytesWritten += pos;
                            if (cachedFrameSize > 0) {
                                currentPositionUs = playbackBaseUs + (bytesWritten / cachedFrameSize) * 1_000_000L / sampleRate;
                            }
                        }
                    }
                } catch (InterruptedException e) {
                    break;
                }
            }
        }, "AudioEngine-Output");
        outputThread.start();
    }

    public void pause() {
        synchronized (outputLock) {
            if (output != null && playing) {
                output.pause();
                playing = false;
            }
        }
    }

    public void resume() {
        synchronized (outputLock) {
            if (output != null && !playing && prepared) {
                output.resume();
                playing = true;
            }
        }
        synchronized (pauseLock) {
            pauseLock.notifyAll();
        }
    }

    public void seekTo(int positionMs) {
        if (pcmBuffer == null) return;

        if (isDsd) {
            seekDsd(positionMs);
            return;
        }

        if (extractor == null || codec == null) return;

        seeking = true;
        long targetUs = (long) positionMs * 1000;
        seekTargetUs = targetUs;
        currentPositionUs = targetUs;  // immediate UI update

        if (streamDataSource != null) {
            // Stream seek must happen off the UI thread (network I/O)
            // Coalescing loop: if rapid seeks arrive, only the latest is executed
            seekExecutor.execute(() -> {
                while (true) {
                    long target = seekTargetUs;
                    if (target < 0 || stopped) break;
                    seekTargetUs = -1;
                    synchronized (codecLock) {
                        if (stopped) break;
                        pcmBuffer.flush();
                        if (eqProcessor != null) eqProcessor.reset();
                        synchronized (outputLock) {
                            if (output != null) {
                                output.pause();
                                output.flush();
                                output.resume();
                            }
                        }
                        codec.flush();
                        extractor.seekTo(target, MediaExtractor.SEEK_TO_PREVIOUS_SYNC);
                        inputDone = false;
                        playbackBaseUs = target;
                        bytesWritten = 0;
                        currentPositionUs = target;
                    }
                    if (seekTargetUs < 0) break;
                }
                seeking = false;
                synchronized (pauseLock) {
                    pauseLock.notifyAll();
                }
            });
        } else {
            // Local file seek -- no network I/O, safe on any thread
            synchronized (codecLock) {
                pcmBuffer.flush();
                if (eqProcessor != null) eqProcessor.reset();
                synchronized (outputLock) {
                    if (output != null) {
                        output.pause();
                        output.flush();
                        output.resume();
                    }
                }
                codec.flush();
                extractor.seekTo(targetUs, MediaExtractor.SEEK_TO_PREVIOUS_SYNC);
                inputDone = false;
                playbackBaseUs = targetUs;
                bytesWritten = 0;
                currentPositionUs = targetUs;
            }
            seeking = false;
            synchronized (pauseLock) {
                pauseLock.notifyAll();
            }
        }
    }

    public void stop() {
        Log.d(TAG, "stop");
        stopped = true;
        playing = false;
        prepared = false;
        seekTargetUs = -1;
        cancelNext();
        seekExecutor.shutdownNow();
        synchronized (pauseLock) {
            pauseLock.notifyAll();
        }

        if (pcmBuffer != null) {
            pcmBuffer.flush();
        }

        if (decodeThread != null) {
            decodeThread.interrupt();
            try { decodeThread.join(500); } catch (InterruptedException ignored) {}
            decodeThread = null;
        }

        if (outputThread != null) {
            outputThread.interrupt();
            try { outputThread.join(500); } catch (InterruptedException ignored) {}
            outputThread = null;
        }

        synchronized (outputLock) {
            if (output != null) {
                output.stop();
                output.release();
                output = null;
            }
        }

        synchronized (codecLock) {
            if (codec != null) {
                try {
                    codec.stop();
                    codec.release();
                } catch (IllegalStateException ignored) {}
                codec = null;
            }

            if (extractor != null) {
                extractor.release();
                extractor = null;
            }
            streamDataSource = null;
        }

        closeDsdResources();
        if (pcmBuffer != null) {
            pcmBuffer.reset();
        }
        // Keep pcmBuffer alive for reuse -- only destroy in release()
        mime = null;
        codecName = null;
    }

    // -- Gapless: next track pre-decode --

    public void queueNext(Context context, Uri uri) {
        cancelNext();
        nextStopped = false;

        new Thread(() -> {
            try {
                String path = uri.getPath();
                if (path != null) {
                    String lower = path.toLowerCase(Locale.ROOT);
                    if (lower.endsWith(".dsf") || lower.endsWith(".dff")) {
                        Log.d(TAG, "queueNext: DSD not supported for gapless");
                        return;
                    }
                }

                MediaExtractor ext = new MediaExtractor();
                ext.setDataSource(context, uri, null);
                if (nextStopped) { ext.release(); return; }

                int audioTrackIndex = -1;
                String mimeLocal = null;
                MediaFormat format = null;
                for (int i = 0; i < ext.getTrackCount(); i++) {
                    format = ext.getTrackFormat(i);
                    mimeLocal = format.getString(MediaFormat.KEY_MIME);
                    if (mimeLocal != null && mimeLocal.startsWith("audio/")) {
                        audioTrackIndex = i;
                        break;
                    }
                }
                if (audioTrackIndex < 0) { ext.release(); return; }
                ext.selectTrack(audioTrackIndex);

                int sr = format.getInteger(MediaFormat.KEY_SAMPLE_RATE);
                int ch = format.getInteger(MediaFormat.KEY_CHANNEL_COUNT);
                long dur = format.containsKey(MediaFormat.KEY_DURATION)
                        ? format.getLong(MediaFormat.KEY_DURATION) : 0;
                int srcBd = detectBitDepth(format);

                int delay = 0, padding = 0;
                if (format.containsKey("encoder-delay")) {
                    delay = format.getInteger("encoder-delay");
                }
                if (format.containsKey("encoder-padding")) {
                    padding = format.getInteger("encoder-padding");
                }

                if (nextStopped) { ext.release(); return; }

                if (isLosslessMime(mimeLocal) && srcBd > 16) {
                    format.setInteger(MediaFormat.KEY_PCM_ENCODING, AudioFormat.ENCODING_PCM_FLOAT);
                }

                MediaCodec cod = MediaCodec.createDecoderByType(mimeLocal);
                String codName = cod.getName();
                cod.configure(format, null, null, 0);
                cod.start();
                if (nextStopped) { cod.stop(); cod.release(); ext.release(); return; }

                MediaFormat outFmt = cod.getOutputFormat();
                int enc = AudioFormat.ENCODING_PCM_16BIT;
                if (outFmt.containsKey(MediaFormat.KEY_PCM_ENCODING)) {
                    enc = outFmt.getInteger(MediaFormat.KEY_PCM_ENCODING);
                }

                NativePcmBuffer buf;
                if (nextPcmBufferPool != null) {
                    buf = nextPcmBufferPool;
                    buf.reset();
                    nextPcmBufferPool = null;
                } else {
                    buf = new NativePcmBuffer(PCM_BUFFER_SIZE);
                }
                if (nextStopped) { cod.stop(); cod.release(); ext.release(); return; }

                nextExtractor = ext;
                nextCodec = cod;
                nextPcmBuffer = buf;
                nextSampleRate = sr;
                nextChannelCount = ch;
                nextEncoding = enc;
                nextSourceBitDepth = srcBd;
                nextDurationUs = dur;
                nextMime = mimeLocal;
                nextCodecName = codName;
                nextEncoderDelay = delay;
                nextEncoderPadding = padding;

                nextDecodeThread = createNextDecodeThread(buf, cod, ext, delay, padding, ch, enc);
                nextDecodeThread.start();

                Log.d(TAG, "queueNext: ready " + sr + "Hz/" + ch + "ch"
                        + " delay=" + delay + " padding=" + padding);

            } catch (Exception e) {
                Log.e(TAG, "queueNext failed", e);
                releaseNextResources();
            }
        }, "AudioEngine-QueueNext").start();
    }

    public void queueNextStream(MediaDataSource dataSource, long durationHintUs) {
        cancelNext();
        nextStopped = false;

        new Thread(() -> {
            try {
                MediaExtractor ext = new MediaExtractor();
                ext.setDataSource(dataSource);
                if (nextStopped) { ext.release(); return; }

                int audioTrackIndex = -1;
                String mimeLocal = null;
                MediaFormat format = null;
                for (int i = 0; i < ext.getTrackCount(); i++) {
                    format = ext.getTrackFormat(i);
                    mimeLocal = format.getString(MediaFormat.KEY_MIME);
                    if (mimeLocal != null && mimeLocal.startsWith("audio/")) {
                        audioTrackIndex = i;
                        break;
                    }
                }
                if (audioTrackIndex < 0) { ext.release(); return; }
                ext.selectTrack(audioTrackIndex);

                int sr = format.getInteger(MediaFormat.KEY_SAMPLE_RATE);
                int ch = format.getInteger(MediaFormat.KEY_CHANNEL_COUNT);
                long dur = format.containsKey(MediaFormat.KEY_DURATION)
                        ? format.getLong(MediaFormat.KEY_DURATION) : 0;
                if (dur <= 0 && durationHintUs > 0) dur = durationHintUs;
                int srcBd = detectBitDepth(format);

                int delay = 0, padding = 0;
                if (format.containsKey("encoder-delay")) {
                    delay = format.getInteger("encoder-delay");
                }
                if (format.containsKey("encoder-padding")) {
                    padding = format.getInteger("encoder-padding");
                }

                if (nextStopped) { ext.release(); return; }

                if (isLosslessMime(mimeLocal) && srcBd > 16) {
                    format.setInteger(MediaFormat.KEY_PCM_ENCODING, AudioFormat.ENCODING_PCM_FLOAT);
                }

                MediaCodec cod = MediaCodec.createDecoderByType(mimeLocal);
                String codName = cod.getName();
                cod.configure(format, null, null, 0);
                cod.start();
                if (nextStopped) { cod.stop(); cod.release(); ext.release(); return; }

                MediaFormat outFmt = cod.getOutputFormat();
                int enc = AudioFormat.ENCODING_PCM_16BIT;
                if (outFmt.containsKey(MediaFormat.KEY_PCM_ENCODING)) {
                    enc = outFmt.getInteger(MediaFormat.KEY_PCM_ENCODING);
                }

                NativePcmBuffer buf;
                if (nextPcmBufferPool != null) {
                    buf = nextPcmBufferPool;
                    buf.reset();
                    nextPcmBufferPool = null;
                } else {
                    buf = new NativePcmBuffer(PCM_BUFFER_SIZE);
                }
                if (nextStopped) { cod.stop(); cod.release(); ext.release(); return; }

                nextExtractor = ext;
                nextCodec = cod;
                nextPcmBuffer = buf;
                nextStreamDataSource = dataSource;
                nextSampleRate = sr;
                nextChannelCount = ch;
                nextEncoding = enc;
                nextSourceBitDepth = srcBd;
                nextDurationUs = dur;
                nextMime = mimeLocal;
                nextCodecName = codName;
                nextEncoderDelay = delay;
                nextEncoderPadding = padding;

                nextDecodeThread = createNextDecodeThread(buf, cod, ext, delay, padding, ch, enc);
                nextDecodeThread.start();

                Log.d(TAG, "queueNextStream: ready " + sr + "Hz/" + ch + "ch");

            } catch (Exception e) {
                Log.e(TAG, "queueNextStream failed", e);
                releaseNextResources();
            }
        }, "AudioEngine-QueueNextStream").start();
    }

    public void cancelNext() {
        nextStopped = true;
        if (nextPcmBuffer != null) {
            nextPcmBuffer.flush();
        }
        if (nextDecodeThread != null) {
            nextDecodeThread.interrupt();
            try { nextDecodeThread.join(500); } catch (InterruptedException ignored) {}
            nextDecodeThread = null;
        }
        releaseNextResources();
    }

    private void releaseNextResources() {
        if (nextCodec != null) {
            try { nextCodec.stop(); nextCodec.release(); } catch (Exception ignored) {}
            nextCodec = null;
        }
        if (nextExtractor != null) {
            nextExtractor.release();
            nextExtractor = null;
        }
        if (nextPcmBuffer != null) {
            // Pool for reuse instead of destroying
            if (nextPcmBufferPool == null) {
                nextPcmBuffer.reset();
                nextPcmBufferPool = nextPcmBuffer;
            } else {
                nextPcmBuffer.destroy();
            }
        }
        nextPcmBuffer = null;
        nextStreamDataSource = null;
        nextMime = null;
        nextCodecName = null;
    }

    private Thread createNextDecodeThread(NativePcmBuffer buffer, MediaCodec cod, MediaExtractor ext,
                                           int delay, int padding, int ch, int enc) {
        return new Thread(() -> {
            MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
            boolean localInputDone = false;
            boolean outputDone = false;
            byte[] decodeBuf = null;

            int frameSize = ch * bytesPerSampleForEncoding(enc);
            NativeGaplessDecoder gaplessDecoder = new NativeGaplessDecoder(
                    delay, padding, frameSize, buffer);

            while (!nextStopped && !outputDone) {
                int decodeSize = 0;
                boolean endOfStream = false;

                try {
                    if (!localInputDone) {
                        int inputIndex = cod.dequeueInputBuffer(10000);
                        if (inputIndex >= 0) {
                            ByteBuffer inputBuf = cod.getInputBuffer(inputIndex);
                            int sampleSize = ext.readSampleData(inputBuf, 0);
                            if (sampleSize < 0) {
                                cod.queueInputBuffer(inputIndex, 0, 0, 0,
                                        MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                                localInputDone = true;
                            } else {
                                long pts = ext.getSampleTime();
                                cod.queueInputBuffer(inputIndex, 0, sampleSize, pts, 0);
                                ext.advance();
                            }
                        }
                    }

                    int outputIndex = cod.dequeueOutputBuffer(info, 10000);
                    if (outputIndex >= 0) {
                        if ((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) {
                            endOfStream = true;
                            cod.releaseOutputBuffer(outputIndex, false);
                        } else if (info.size > 0) {
                            ByteBuffer outputBuf = cod.getOutputBuffer(outputIndex);
                            if (decodeBuf == null || decodeBuf.length < info.size) {
                                decodeBuf = new byte[info.size];
                            }
                            outputBuf.get(decodeBuf, 0, info.size);
                            decodeSize = info.size;
                            cod.releaseOutputBuffer(outputIndex, false);
                        } else {
                            cod.releaseOutputBuffer(outputIndex, false);
                        }
                    }
                } catch (Exception e) {
                    if (!nextStopped) Log.e(TAG, "next decode error", e);
                    break;
                }

                if (endOfStream) {
                    outputDone = true;
                    gaplessDecoder.signalEnd();
                } else if (decodeSize > 0) {
                    try {
                        gaplessDecoder.processFrame(decodeBuf, 0, decodeSize);
                    } catch (InterruptedException e) {
                        break;
                    }
                }
            }
            gaplessDecoder.destroy();
        }, "AudioEngine-NextDecode");
    }

    private static int detectBitDepth(MediaFormat format) {
        if (format.containsKey("bits-per-sample")) {
            return format.getInteger("bits-per-sample");
        } else if (format.containsKey("bit-width")) {
            return format.getInteger("bit-width");
        } else if (format.containsKey(MediaFormat.KEY_PCM_ENCODING)) {
            int srcEnc = format.getInteger(MediaFormat.KEY_PCM_ENCODING);
            switch (srcEnc) {
                case AudioFormat.ENCODING_PCM_FLOAT:
                case AudioFormat.ENCODING_PCM_32BIT:
                    return 32;
                case AudioFormat.ENCODING_PCM_24BIT_PACKED:
                    return 24;
                case AudioFormat.ENCODING_PCM_16BIT:
                    return 16;
            }
        }
        return 16;
    }

    private static boolean isLosslessMime(String mime) {
        return "audio/flac".equals(mime)
            || "audio/alac".equals(mime)
            || "audio/raw".equals(mime);
    }

    private static int bytesPerSampleForEncoding(int encoding) {
        switch (encoding) {
            case AudioFormat.ENCODING_PCM_FLOAT: return 4;
            case AudioFormat.ENCODING_PCM_32BIT: return 4;
            case AudioFormat.ENCODING_PCM_24BIT_PACKED: return 3;
            default: return 2;
        }
    }

    public void release() {
        Log.d(TAG, "release");
        stop();
        if (pcmBuffer != null) {
            pcmBuffer.destroy();
            pcmBuffer = null;
        }
        if (nextPcmBufferPool != null) {
            nextPcmBufferPool.destroy();
            nextPcmBufferPool = null;
        }
        released = true;
    }

    public boolean isPlaying() {
        return playing;
    }

    public int getCurrentPosition() {
        return (int) (currentPositionUs / 1000);
    }

    public int getDuration() {
        return (int) (durationUs / 1000);
    }

    private void notifyError(String message) {
        Log.e(TAG, message);
        if (onErrorListener != null) {
            onErrorListener.onError(this, message);
        }
    }
}
