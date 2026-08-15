package dev.darwinart.probe;

import android.content.AttributionSource;
import android.content.ContentResolver;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.pm.ApplicationInfo;
import android.content.res.Resources;

import java.lang.reflect.Constructor;

public final class ProbeContext extends ContextWrapper {
    private final ApplicationInfo applicationInfo;
    private final AttributionSource attributionSource;
    private final ContentResolver contentResolver;
    private final Resources resources;

    public ProbeContext(Resources resources) {
        super(null);
        this.resources = resources;
        applicationInfo = new ApplicationInfo();
        applicationInfo.targetSdkVersion = 36;
        attributionSource = new AttributionSource.Builder(1000)
                .setPackageName(getPackageName())
                .build();
        contentResolver = new ProbeContentResolver(this);
    }

    @Override
    public ApplicationInfo getApplicationInfo() {
        return applicationInfo;
    }

    @Override
    public ContentResolver getContentResolver() {
        return contentResolver;
    }

    @Override
    public AttributionSource getAttributionSource() {
        return attributionSource;
    }

    @Override
    public Resources getResources() {
        return resources;
    }

    @Override
    public Object getSystemService(String name) {
        if (LAYOUT_INFLATER_SERVICE.equals(name)) {
            return construct("com.android.internal.policy.PhoneLayoutInflater");
        }
        if (WINDOW_SERVICE.equals(name)) {
            return construct("android.view.WindowManagerImpl");
        }
        return null;
    }

    @Override
    public String getPackageName() {
        return "dev.darwinart.probe";
    }

    @Override
    public String getOpPackageName() {
        return getPackageName();
    }

    // Hidden on the SDK surface but virtual in the platform Context class.
    public int getUserId() {
        return 0;
    }

    private Object construct(String className) {
        try {
            Class<?> type = Class.forName(className);
            Constructor<?> constructor = type.getConstructor(Context.class);
            return constructor.newInstance(this);
        } catch (ReflectiveOperationException error) {
            throw new IllegalStateException("Could not construct " + className, error);
        }
    }
}
