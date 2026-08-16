package dev.darwinart.probe;

import android.content.AttributionSource;
import android.content.ContentResolver;
import android.content.ContentCaptureOptions;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.res.Resources;
import android.os.Looper;
import android.view.autofill.AutofillManager;

import java.lang.reflect.Constructor;

public final class ProbeContext extends ContextWrapper {
    private final ApplicationInfo applicationInfo;
    private final AttributionSource attributionSource;
    private final ContentResolver contentResolver;
    private final Resources resources;
    private final Resources.Theme theme;
    private final PackageManager packageManager;

    public ProbeContext(Resources resources, PackageManager packageManager) {
        super(null);
        this.resources = resources;
        theme = resources.newTheme();
        this.packageManager = packageManager;
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
    public Context getApplicationContext() {
        // PhoneWindow explicitly supports processes without an application
        // context by using the Activity context directly for its DecorView.
        return null;
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
    public Resources.Theme getTheme() {
        return theme;
    }

    @Override
    public PackageManager getPackageManager() {
        return packageManager;
    }

    @Override
    public Looper getMainLooper() {
        return Looper.getMainLooper();
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

    // Hidden Context hooks called by Activity.attachBaseContext(). The Darwin
    // host does not expose autofill or content-capture services, so ownership
    // stays with the attached Activity while these services remain disabled.
    public void setAutofillClient(AutofillManager.AutofillClient client) {}

    public void setContentCaptureOptions(ContentCaptureOptions options) {}

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
