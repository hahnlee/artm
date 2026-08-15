package dev.darwinart.probe;

public final class Hello {
    public static native int hostPageSize();

    public static void main(String[] args) {
        System.out.println(args[0]);
    }

    public static int answer() {
        return 42;
    }

    public static int nativeRoundTrip() {
        return hostPageSize() == 16384 ? 42 : -1;
    }

    public static int runtimeNativeArraycopy() {
        int[] source = new int[] { 19, 23 };
        int[] destination = new int[2];
        System.arraycopy(source, 0, destination, 0, source.length);
        return destination[0] + destination[1];
    }
}
