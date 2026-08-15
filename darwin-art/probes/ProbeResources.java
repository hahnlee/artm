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

    public ProbeResources(AssetManager assets) {
        super(assets, DISPLAY_METRICS, CONFIGURATION);
    }

    @Override
    public boolean getBoolean(int id) {
        return false;
    }

    @Override
    public int getDimensionPixelSize(int id) {
        return 8;
    }

    @Override
    public int getColor(int id) {
        return 0xff2563eb;
    }

    @Override
    public int getColor(int id, Theme theme) {
        return 0xff2563eb;
    }

    @Override
    public int getInteger(int id) {
        // ViewConfiguration's config_overrideHasPermanentMenuKey=false value;
        // this avoids consulting a remote IWindowManager during the host gate.
        return 2;
    }

    @Override
    public XmlResourceParser getAnimation(int id) {
        return new ProbeXmlResourceParser();
    }

    @Override
    public void getValue(int id, TypedValue outValue, boolean resolveRefs) {
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
