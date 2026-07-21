package com.nerio.audioengine;

/**
 * Volume control mode selected by the user (Settings) or auto-resolved at runtime.
 *
 * AUTO     - Prefer hardware (UAC Feature Unit) when present; fall back to software gain
 *            for PCM. DSD has no software fallback (1-bit signal cannot be attenuated).
 * HARDWARE - Force the UAC Feature Unit path. Slider greys out if the DAC doesn't expose one.
 * SOFTWARE - Force the software gain path. PCM only; DSD plays at unity gain.
 * EXTERNAL - Tell the app to keep its hands off: DAC is set to unity once, slider is decorative.
 *            For setups with a downstream analog amp / DAC physical knob doing the attenuation.
 */
public enum VolumeMode {
    AUTO,
    HARDWARE,
    SOFTWARE,
    EXTERNAL;

    public static VolumeMode fromString(String s) {
        if (s == null) return AUTO;
        try {
            return VolumeMode.valueOf(s.toUpperCase());
        } catch (IllegalArgumentException e) {
            return AUTO;
        }
    }
}
