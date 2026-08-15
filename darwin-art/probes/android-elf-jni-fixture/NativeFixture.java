package darwin.art.nativefixture;

public final class NativeFixture {
    private NativeFixture() {}

    public static native long nativeAdd(int left, long middle, int right);

    public static native long nativeSpill(
            boolean z,
            byte b,
            char c,
            short s,
            int i,
            long j,
            Object reference,
            float f0,
            double d0,
            float f1,
            double d1,
            float f2,
            double d2,
            float f3,
            double d3,
            float f4,
            float f5,
            double d4);
}
