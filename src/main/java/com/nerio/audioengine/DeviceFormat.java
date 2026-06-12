package com.nerio.audioengine;

/** One (sample rate, channel count, bit depth) combination a device supports. */
public final class DeviceFormat {

    public final int rate;
    public final int channels;
    public final int bits;

    public DeviceFormat(int rate, int channels, int bits) {
        this.rate = rate;
        this.channels = channels;
        this.bits = bits;
    }

    @Override
    public String toString() {
        return rate + "Hz/" + bits + "bit/" + channels + "ch";
    }
}
