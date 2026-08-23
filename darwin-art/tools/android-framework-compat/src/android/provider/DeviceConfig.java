package android.provider;

import java.util.concurrent.Executor;

/**
 * Minimal no-service DeviceConfig seam for the detached framework runtime.
 * Android's boot framework references this class from widget initialization,
 * but the Darwin host intentionally has no DeviceConfig service manager.
 * Returning the caller's defaults preserves the AOSP feature-flag contract.
 */
public final class DeviceConfig {
    private DeviceConfig() {}

    public interface OnPropertiesChangedListener {
        void onPropertiesChanged(Properties properties);
    }

    public static final class Properties {
        public String getString(String key, String fallback) { return fallback; }
        public boolean getBoolean(String key, boolean fallback) { return fallback; }
        public int getInt(String key, int fallback) { return fallback; }
        public long getLong(String key, long fallback) { return fallback; }
        public float getFloat(String key, float fallback) { return fallback; }
    }

    public static String getString(String namespace, String key, String fallback) {
        return fallback;
    }

    public static boolean getBoolean(String namespace, String key, boolean fallback) {
        return fallback;
    }

    public static int getInt(String namespace, String key, int fallback) {
        return fallback;
    }

    public static long getLong(String namespace, String key, long fallback) {
        return fallback;
    }

    public static float getFloat(String namespace, String key, float fallback) {
        return fallback;
    }

    public static Properties getProperties(String namespace, String... keys) {
        return new Properties();
    }

    public static void addOnPropertiesChangedListener(
            String namespace, Executor executor, OnPropertiesChangedListener listener) {}

    public static void removeOnPropertiesChangedListener(OnPropertiesChangedListener listener) {}
}
