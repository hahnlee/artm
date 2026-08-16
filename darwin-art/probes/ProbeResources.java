package dev.darwinart.probe;

import android.content.res.AssetManager;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.content.res.XmlResourceParser;
import android.util.DisplayMetrics;
import android.util.TypedValue;

@SuppressWarnings("deprecation")
public final class ProbeResources extends Resources {
    private static final Configuration CONFIGURATION = new Configuration();
    private static final DisplayMetrics DISPLAY_METRICS = new DisplayMetrics();

    static {
        CONFIGURATION.setToDefaults();
        DISPLAY_METRICS.setToDefaults();
    }

    private final boolean frameworkResources;

    public static void configureDisplayScale(int scale) {
        if (scale != 1 && scale != 2) {
            throw new IllegalArgumentException("display scale must be 1 or 2");
        }
        int densityDpi = DisplayMetrics.DENSITY_DEFAULT * scale;
        CONFIGURATION.setToDefaults();
        CONFIGURATION.densityDpi = densityDpi;
        DISPLAY_METRICS.setToDefaults();
        DISPLAY_METRICS.density = scale;
        DISPLAY_METRICS.densityDpi = densityDpi;
        DISPLAY_METRICS.scaledDensity = scale;
        DISPLAY_METRICS.xdpi = densityDpi;
        DISPLAY_METRICS.ydpi = densityDpi;
    }

    public ProbeResources(AssetManager assets, boolean frameworkResources) {
        super(assets, DISPLAY_METRICS, CONFIGURATION);
        this.frameworkResources = frameworkResources;
    }

    @Override
    public boolean getBoolean(int id) {
        return frameworkResources ? super.getBoolean(id) : false;
    }

    @Override
    public int getDimensionPixelSize(int id) {
        return frameworkResources ? super.getDimensionPixelSize(id) : 8;
    }

    @Override
    public int getColor(int id) {
        return frameworkResources ? super.getColor(id) : 0xff2563eb;
    }

    @Override
    public int getColor(int id, Theme theme) {
        return frameworkResources ? super.getColor(id, theme) : 0xff2563eb;
    }

    @Override
    public int getInteger(int id) {
        // ViewConfiguration's config_overrideHasPermanentMenuKey=false value;
        // this avoids consulting a remote IWindowManager during the host gate.
        return 2;
    }

    @Override
    public XmlResourceParser getAnimation(int id) {
        return frameworkResources ? super.getAnimation(id) : new ProbeXmlResourceParser();
    }

    @Override
    public void getValue(int id, TypedValue outValue, boolean resolveRefs) {
        if (frameworkResources) {
            super.getValue(id, outValue, resolveRefs);
            return;
        }
        outValue.type = TypedValue.TYPE_FLOAT;
        outValue.data = Float.floatToRawIntBits(2.0f);
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
