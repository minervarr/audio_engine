package com.nerio.audioengine;

/**
 * Picks the highest-fidelity format a device supports, so apps can offer a
 * "best available" default instead of hardcoding rates.
 *
 * <p>Policy: bit depth wins over sample rate (24-bit/96 kHz beats
 * 16-bit/384 kHz — word length buys real dynamic range, ultrasonic rate
 * does not), and rate breaks ties. Channel count is matched to the request:
 * exact if possible, else the smallest supported count above it, else the
 * largest available.
 */
public final class FormatSelector {

    private FormatSelector() {}

    /** Best capture-direction format, from the device's full format tuples. */
    public static DeviceFormat bestCaptureFormat(UsbAudioDevice device, int desiredChannels) {
        if (device == null || !device.isValid()) return null;
        DeviceFormat best = bestFormat(
                UsbAudioNative.nativeGetCaptureFormats(device.getNativeHandle()),
                desiredChannels);
        if (best != null) return best;
        UsbAudioInput in = device.getInput();
        return bestFromAxes(in.getSupportedRates(), in.getSupportedChannelCounts(),
                in.getSupportedBitDepths(), desiredChannels);
    }

    /** Best output-direction (playback) format, from the device's full format tuples. */
    public static DeviceFormat bestOutputFormat(UsbAudioDevice device, int desiredChannels) {
        if (device == null || !device.isValid()) return null;
        DeviceFormat best = bestFormat(
                UsbAudioNative.nativeGetOutputFormats(device.getNativeHandle()),
                desiredChannels);
        if (best != null) return best;
        UsbAudioOutput out = device.getOutput();
        return bestFromAxes(out.getSupportedOutputRates(), out.getSupportedOutputChannelCounts(),
                out.getSupportedOutputBitDepths(), desiredChannels);
    }

    /**
     * Best format from flattened (rate, channels, bits) tuples, 3 ints each.
     * Returns null when {@code tuples} is null, empty, or malformed.
     */
    public static DeviceFormat bestFormat(int[] tuples, int desiredChannels) {
        if (tuples == null || tuples.length < 3) return null;

        int chosenCh = chooseChannels(tuples, desiredChannels);
        DeviceFormat best = null;
        for (int i = 0; i + 2 < tuples.length; i += 3) {
            int r = tuples[i], c = tuples[i + 1], b = tuples[i + 2];
            if (c != chosenCh) continue;
            if (best == null || b > best.bits || (b == best.bits && r > best.rate)) {
                best = new DeviceFormat(r, c, b);
            }
        }
        return best;
    }

    private static int chooseChannels(int[] tuples, int desired) {
        int above = 0;  // smallest count > desired
        int max = 0;
        for (int i = 1; i + 1 < tuples.length; i += 3) {
            int c = tuples[i];
            if (c == desired) return c;
            if (c > desired && (above == 0 || c < above)) above = c;
            if (c > max) max = c;
        }
        return above != 0 ? above : max;
    }

    /**
     * Fallback when tuples are unavailable: combine the per-axis lists. The
     * axes are independent, so the combination may not exist — callers should
     * configure() and read back the negotiated values (the native relax-match
     * lands on the closest real alt-setting).
     */
    private static DeviceFormat bestFromAxes(int[] rates, int[] channelCounts,
                                             int[] bitDepths, int desiredChannels) {
        if (rates == null || rates.length == 0) return null;
        int rate = max(rates);
        int bits = (bitDepths != null && bitDepths.length > 0) ? max(bitDepths) : 24;
        int ch = desiredChannels;
        if (channelCounts != null && channelCounts.length > 0) {
            int above = 0, maxCh = 0;
            boolean exact = false;
            for (int c : channelCounts) {
                if (c == desiredChannels) exact = true;
                if (c > desiredChannels && (above == 0 || c < above)) above = c;
                if (c > maxCh) maxCh = c;
            }
            if (!exact) ch = above != 0 ? above : maxCh;
        }
        return new DeviceFormat(rate, ch, bits);
    }

    private static int max(int[] values) {
        int m = values[0];
        for (int v : values) if (v > m) m = v;
        return m;
    }
}
