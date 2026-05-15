package com.nerio.audioengine;

public enum DsdMode {
    AUTO,
    NATIVE,
    DOP,
    PCM;

    public static DsdMode fromString(String s) {
        if (s == null) return AUTO;
        try {
            return valueOf(s.trim().toUpperCase());
        } catch (IllegalArgumentException e) {
            return AUTO;
        }
    }
}
