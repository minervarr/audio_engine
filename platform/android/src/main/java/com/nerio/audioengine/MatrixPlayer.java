package com.nerio.audioengine;

import android.content.Context;
import android.media.MediaDataSource;
import android.net.Uri;

import java.util.List;

/**
 * Simple facade around {@link AudioEngine} for common playback use cases.
 *
 * <p>Creates an {@link AudioTrackOutput} (speaker) by default. Call
 * {@link #switchOutput(AudioOutput)} to route audio to a USB DAC or custom output.
 *
 * <p>For advanced control (thread model, direct buffer access), use
 * {@link #getEngine()} to access the underlying AudioEngine directly.
 */
public class MatrixPlayer {

    private final AudioEngine engine;
    private EqProcessor eqProcessor;
    private EqProfile activeProfile;

    // Facade listener types -- use MatrixPlayer instead of AudioEngine
    public interface OnPreparedListener {
        void onPrepared(MatrixPlayer player);
    }

    public interface OnCompletionListener {
        void onCompletion(MatrixPlayer player);
    }

    public interface OnErrorListener {
        void onError(MatrixPlayer player, String message);
    }

    public interface OnTrackTransitionListener {
        void onTransition(MatrixPlayer player);
    }

    private OnPreparedListener onPreparedListener;
    private OnCompletionListener onCompletionListener;
    private OnErrorListener onErrorListener;
    private OnTrackTransitionListener onTrackTransitionListener;

    public MatrixPlayer() {
        engine = new AudioEngine();
        engine.switchOutput(new AudioTrackOutput());

        engine.setOnPreparedListener(e -> {
            recalculateEq();
            if (onPreparedListener != null) {
                onPreparedListener.onPrepared(this);
            }
        });

        engine.setOnCompletionListener(e -> {
            if (onCompletionListener != null) {
                onCompletionListener.onCompletion(this);
            }
        });

        engine.setOnErrorListener((e, msg) -> {
            if (onErrorListener != null) {
                onErrorListener.onError(this, msg);
            }
        });

        engine.setOnTransitionListener(e -> {
            recalculateEq();
            if (onTrackTransitionListener != null) {
                onTrackTransitionListener.onTransition(this);
            }
        });
    }

    // -- Playback --

    public void play(Context context, Uri uri) {
        engine.play(context, uri);
    }

    public void playStream(MediaDataSource dataSource, long durationHintUs) {
        engine.playStream(dataSource, durationHintUs);
    }

    public void pause() {
        engine.pause();
    }

    public void resume() {
        engine.resume();
    }

    public void togglePlayPause() {
        if (engine.isPlaying()) {
            engine.pause();
        } else {
            engine.resume();
        }
    }

    public void seekTo(int positionMs) {
        engine.seekTo(positionMs);
    }

    public void stop() {
        engine.stop();
    }

    public void release() {
        if (eqProcessor != null) {
            eqProcessor.destroy();
            eqProcessor = null;
        }
        engine.release();
    }

    // -- Gapless --

    public void queueNext(Context context, Uri uri) {
        engine.queueNext(context, uri);
    }

    public void queueNextStream(MediaDataSource dataSource, long durationHintUs) {
        engine.queueNextStream(dataSource, durationHintUs);
    }

    public void cancelNext() {
        engine.cancelNext();
    }

    // -- Output --

    public boolean switchOutput(AudioOutput output) {
        return engine.switchOutput(output);
    }

    // -- EQ --

    /**
     * Set an EQ profile. Pass null to disable EQ processing.
     * Coefficients are recalculated automatically when the sample rate is known
     * (after prepare or track transition).
     */
    public void setEqProfile(EqProfile profile) {
        activeProfile = profile;
        if (profile == null) {
            if (eqProcessor != null) {
                eqProcessor.setEnabled(false);
                engine.setEqProcessor(null);
            }
            return;
        }
        ensureEqProcessor();
        recalculateEq();
    }

    /**
     * Load built-in EQ profiles from the bundled asset.
     */
    public static List<EqProfile> loadEqProfiles(Context context) {
        return EqProfile.loadAll(context);
    }

    private void ensureEqProcessor() {
        if (eqProcessor == null) {
            eqProcessor = new EqProcessor();
        }
        engine.setEqProcessor(eqProcessor);
    }

    private void recalculateEq() {
        if (eqProcessor == null || activeProfile == null) return;
        int sr = engine.getSampleRate();
        if (sr <= 0) return;
        eqProcessor.computeCoefficients(activeProfile, sr,
                engine.getChannelCount(), engine.getEncoding());
        eqProcessor.setEnabled(true);
    }

    // -- State --

    public boolean isPlaying() {
        return engine.isPlaying();
    }

    public int getCurrentPosition() {
        return engine.getCurrentPosition();
    }

    public int getDuration() {
        return engine.getDuration();
    }

    public int getSampleRate() {
        return engine.getSampleRate();
    }

    public int getChannelCount() {
        return engine.getChannelCount();
    }

    public int getEncoding() {
        return engine.getEncoding();
    }

    public int getSourceBitDepth() {
        return engine.getSourceBitDepth();
    }

    public boolean isDsd() {
        return engine.isDsd();
    }

    public String getMime() {
        return engine.getMime();
    }

    public String getCodecName() {
        return engine.getCodecName();
    }

    public SignalPathInfo getSignalPathInfo() {
        SignalPathInfo info = new SignalPathInfo();
        info.sourceRate = engine.getSampleRate();
        info.sourceChannels = engine.getChannelCount();
        info.sourceBitDepth = engine.getSourceBitDepth();
        info.sourceMime = engine.getMime();
        info.codecName = engine.getCodecName();
        info.decodedEncoding = engine.getEncoding();
        info.isDsd = engine.isDsd();
        info.dsdRate = engine.getDsdRate();

        if (eqProcessor != null && activeProfile != null) {
            info.eqActive = true;
            info.eqProfileName = activeProfile.name;
        }

        return info;
    }

    // -- Listeners --

    public void setOnPreparedListener(OnPreparedListener listener) {
        this.onPreparedListener = listener;
    }

    public void setOnCompletionListener(OnCompletionListener listener) {
        this.onCompletionListener = listener;
    }

    public void setOnErrorListener(OnErrorListener listener) {
        this.onErrorListener = listener;
    }

    public void setOnTrackTransitionListener(OnTrackTransitionListener listener) {
        this.onTrackTransitionListener = listener;
    }

    // -- Advanced --

    /**
     * Direct access to the underlying AudioEngine for advanced use cases.
     */
    public AudioEngine getEngine() {
        return engine;
    }
}
