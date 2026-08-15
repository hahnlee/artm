package dev.darwinart.probe;

import android.content.res.AssetManager;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.util.DisplayMetrics;

@SuppressWarnings("deprecation")
public final class ProbeResources extends Resources {
    private static final Configuration CONFIGURATION = new Configuration();
    private static final DisplayMetrics DISPLAY_METRICS = new DisplayMetrics();

    static {
        CONFIGURATION.setToDefaults();
        DISPLAY_METRICS.setToDefaults();
    }

    // The Darwin probe allocates this class through JNI without invoking this
    // constructor. The superclass ResourceImpl requires Android AssetManager
    // natives which are intentionally outside the current window gate.
    public ProbeResources() {
        super((AssetManager) null, DISPLAY_METRICS, CONFIGURATION);
    }

    @Override
    public boolean getBoolean(int id) {
        return false;
    }

    @Override
    public Configuration getConfiguration() {
        return CONFIGURATION;
    }

    @Override
    public DisplayMetrics getDisplayMetrics() {
        return DISPLAY_METRICS;
    }
}
