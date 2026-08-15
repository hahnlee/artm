package dev.darwinart.probe;

public final class Hello {
    public static native int hostPageSize();

    public static int answer() {
        return 42;
    }

    public static int nativeRoundTrip() {
        return hostPageSize() == 16384 ? 42 : -1;
    }
}
