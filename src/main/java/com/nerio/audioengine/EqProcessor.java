package com.nerio.audioengine;

import android.util.Log;

public class EqProcessor {

    private static final String TAG = "EqProcessor";

    static {
        System.loadLibrary("matrix_audio");
    }

    private long handle;
    private EqProfile currentProfile;

    public EqProcessor() {
        handle = nativeCreate();
    }

    /**
     * Compute biquad coefficients from profile filter parameters at the given sample rate
     * and configure the native processor.
     *
     * Uses Robert Bristow-Johnson Audio EQ Cookbook formulas.
     */
    public void computeCoefficients(EqProfile profile, int sampleRate, int channelCount, int encoding) {
        if (handle == 0 || profile == null) return;
        this.currentProfile = profile;

        double preampLinear = Math.pow(10.0, profile.preamp / 20.0);
        int numFilters = profile.filters.size();
        double[] coefficients = new double[numFilters * 5];

        for (int i = 0; i < numFilters; i++) {
            EqProfile.Filter f = profile.filters.get(i);
            double[] c = computeBiquad(f.type, f.fc, f.gain, f.q, sampleRate);
            System.arraycopy(c, 0, coefficients, i * 5, 5);
        }

        nativeConfigure(handle, numFilters, coefficients, preampLinear, channelCount, encoding);
        Log.i(TAG, "Configured " + numFilters + " filters for \"" + profile.name
                + "\" at " + sampleRate + "Hz");
    }

    public void process(byte[] data, int offset, int length) {
        if (handle != 0) {
            nativeProcess(handle, data, offset, length);
        }
    }

    public void reset() {
        if (handle != 0) {
            nativeReset(handle);
        }
    }

    public void setEnabled(boolean enabled) {
        if (handle != 0) {
            nativeSetEnabled(handle, enabled);
        }
    }

    public void destroy() {
        if (handle != 0) {
            nativeDestroy(handle);
            handle = 0;
        }
    }

    public EqProfile getCurrentProfile() {
        return currentProfile;
    }

    /**
     * Compute biquad coefficients [b0, b1, b2, a1, a2] (a0 normalized to 1).
     * Robert Bristow-Johnson Audio EQ Cookbook.
     */
    private static double[] computeBiquad(String type, double fc, double gain, double q, int sampleRate) {
        double w0 = 2.0 * Math.PI * fc / sampleRate;
        double cosW0 = Math.cos(w0);
        double sinW0 = Math.sin(w0);
        double A = Math.pow(10.0, gain / 40.0); // sqrt of linear gain
        double alpha = sinW0 / (2.0 * q);

        double b0, b1, b2, a0, a1, a2;

        switch (type) {
            case "LSC": // Low shelf
                double lsBeta = 2.0 * Math.sqrt(A) * alpha;
                b0 =     A * ((A + 1) - (A - 1) * cosW0 + lsBeta);
                b1 = 2 * A * ((A - 1) - (A + 1) * cosW0);
                b2 =     A * ((A + 1) - (A - 1) * cosW0 - lsBeta);
                a0 =          (A + 1) + (A - 1) * cosW0 + lsBeta;
                a1 =    -2 * ((A - 1) + (A + 1) * cosW0);
                a2 =          (A + 1) + (A - 1) * cosW0 - lsBeta;
                break;

            case "HSC": // High shelf
                double hsBeta = 2.0 * Math.sqrt(A) * alpha;
                b0 =     A * ((A + 1) + (A - 1) * cosW0 + hsBeta);
                b1 = -2 * A * ((A - 1) + (A + 1) * cosW0);
                b2 =     A * ((A + 1) + (A - 1) * cosW0 - hsBeta);
                a0 =          (A + 1) - (A - 1) * cosW0 + hsBeta;
                a1 =     2 * ((A - 1) - (A + 1) * cosW0);
                a2 =          (A + 1) - (A - 1) * cosW0 - hsBeta;
                break;

            default: // PK (peaking EQ)
                b0 = 1 + alpha * A;
                b1 = -2 * cosW0;
                b2 = 1 - alpha * A;
                a0 = 1 + alpha / A;
                a1 = -2 * cosW0;
                a2 = 1 - alpha / A;
                break;
        }

        // Normalize by a0
        return new double[]{
                b0 / a0, b1 / a0, b2 / a0,
                a1 / a0, a2 / a0
        };
    }

    // Native methods
    private static native long nativeCreate();
    private static native void nativeDestroy(long handle);
    private static native void nativeConfigure(long handle, int numFilters,
                                                double[] coefficients, double preamp,
                                                int channelCount, int encoding);
    private static native void nativeProcess(long handle, byte[] data, int offset, int length);
    private static native void nativeReset(long handle);
    private static native void nativeSetEnabled(long handle, boolean enabled);
}
