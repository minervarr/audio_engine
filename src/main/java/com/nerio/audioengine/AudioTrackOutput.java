package com.nerio.audioengine;

import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioTrack;
import android.util.Log;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;

public class AudioTrackOutput implements AudioOutput {

    private static final String TAG = "AudioTrackOutput";

    static {
        System.loadLibrary("matrix_audio");
    }

    private AudioTrack audioTrack;
    private boolean floatToInt16;
    private byte[] convBuf;

    @Override
    public boolean configure(int sampleRate, int channelCount, int encoding, int sourceBitDepth) {
        if (sourceBitDepth == 1) {
            Log.d(TAG, "configure: DSD not supported on speaker output");
            return false;
        }

        release();

        // Speaker hardware often can't handle PCM_FLOAT properly (garbled/slow
        // output). Downgrade to PCM_16BIT and convert in the write path.
        // USB output receives float data directly via UsbAudioOutput.
        int trackEncoding = encoding;
        floatToInt16 = false;
        if (encoding == AudioFormat.ENCODING_PCM_FLOAT) {
            trackEncoding = AudioFormat.ENCODING_PCM_16BIT;
            floatToInt16 = true;
            Log.d(TAG, "configure: downgrading PCM_FLOAT -> PCM_16BIT for speaker");
        }

        try {
            int channelMask = channelCount == 1
                    ? AudioFormat.CHANNEL_OUT_MONO
                    : AudioFormat.CHANNEL_OUT_STEREO;

            int minBufSize = AudioTrack.getMinBufferSize(sampleRate, channelMask, trackEncoding);
            int bufSize = Math.max(minBufSize * 4, 16384);

            Log.d(TAG, "configure: " + sampleRate + "Hz/" + channelCount + "ch enc=" + trackEncoding + " buf=" + bufSize);

            audioTrack = new AudioTrack.Builder()
                    .setAudioAttributes(new AudioAttributes.Builder()
                            .setUsage(AudioAttributes.USAGE_MEDIA)
                            .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                            .build())
                    .setAudioFormat(new AudioFormat.Builder()
                            .setSampleRate(sampleRate)
                            .setChannelMask(channelMask)
                            .setEncoding(trackEncoding)
                            .build())
                    .setBufferSizeInBytes(bufSize)
                    .setTransferMode(AudioTrack.MODE_STREAM)
                    .build();
            return true;
        } catch (Exception e) {
            Log.e(TAG, "configure failed", e);
            return false;
        }
    }

    @Override
    public boolean start() {
        if (audioTrack != null) {
            try {
                audioTrack.play();
                Log.d(TAG, "start");
                return true;
            } catch (Exception e) {
                Log.e(TAG, "start failed", e);
                return false;
            }
        }
        return false;
    }

    @Override
    public int write(byte[] data, int offset, int length) {
        if (audioTrack == null) return -1;

        if (floatToInt16) {
            // Convert float32 samples to int16 with TPDF dither via native code.
            // 4 bytes per float sample -> 2 bytes per int16 sample.
            int samples = length / 4;
            int outBytes = samples * 2;
            if (convBuf == null || convBuf.length < outBytes) {
                convBuf = new byte[outBytes];
            }
            nativeFloatToInt16(data, offset, convBuf, samples);
            int written = audioTrack.write(convBuf, 0, outBytes, AudioTrack.WRITE_NON_BLOCKING);
            if (written < 0) return written;
            // Return the number of float input bytes consumed
            return (written / 2) * 4;
        }

        return audioTrack.write(data, offset, length, AudioTrack.WRITE_NON_BLOCKING);
    }

    @Override
    public void pause() {
        if (audioTrack != null) {
            audioTrack.pause();
        }
    }

    @Override
    public void resume() {
        if (audioTrack != null) {
            audioTrack.play();
        }
    }

    @Override
    public void flush() {
        if (audioTrack != null) {
            audioTrack.flush();
        }
    }

    @Override
    public void stop() {
        if (audioTrack != null) {
            Log.d(TAG, "stop");
            try {
                audioTrack.stop();
            } catch (IllegalStateException ignored) {}
        }
    }

    @Override
    public void release() {
        if (audioTrack != null) {
            Log.d(TAG, "release");
            try {
                audioTrack.stop();
            } catch (IllegalStateException ignored) {}
            audioTrack.release();
            audioTrack = null;
        }
        convBuf = null;
    }

    private static native void nativeFloatToInt16(byte[] src, int srcOffset, byte[] dst, int sampleCount);
}
