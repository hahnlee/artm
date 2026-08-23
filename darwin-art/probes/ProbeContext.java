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
    private final String packageName;
    private final ClassLoader classLoader;
    private static volatile ClassLoader applicationClassLoader;

    public ProbeContext(Resources resources, PackageManager packageManager) {
        this(resources, packageManager, "dev.darwinart.probe");
    }

    public ProbeContext(Resources resources, PackageManager packageManager,
            String packageName) {
        super(null);
        this.resources = resources;
        theme = resources.newTheme();
        this.packageManager = packageManager;
        this.packageName = packageName == null ? "dev.darwinart.probe" : packageName;
        // Capture the PathClassLoader at context construction.  The detached
        // host may later execute Activity/View work on a dedicated Java UI
        // thread, so consulting that thread's mutable context loader would
        // otherwise make LayoutInflater lose APK-owned custom views.
        classLoader = ProbeContext.class.getClassLoader();
        applicationInfo = new ApplicationInfo();
        applicationInfo.packageName = this.packageName;
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
        // The detached launcher has no separately constructed Application
        // ContextImpl.  Returning this stable, process-owned context matches
        // the identity callers observe on Android and keeps framework widget
        // construction from dereferencing a null application context.
        return this;
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
    public android.content.res.AssetManager getAssets() {
        return resources.getAssets();
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
        if (INPUT_METHOD_SERVICE.equals(name)) {
            try {
                Class<?> type = Class.forName("android.view.inputmethod.InputMethodManager");
                java.lang.reflect.Method factory = type.getDeclaredMethod(
                        "createStubInstance", int.class, Looper.class);
                factory.setAccessible(true);
                return factory.invoke(null, 0, Looper.getMainLooper());
            } catch (ReflectiveOperationException error) {
                throw new IllegalStateException("Could not construct input method stub", error);
            }
        }
        if (LAYOUT_INFLATER_SERVICE.equals(name)) {
            return construct("com.android.internal.policy.PhoneLayoutInflater");
        }
        if (WINDOW_SERVICE.equals(name)) {
            return construct("android.view.WindowManagerImpl");
        }
        return null;
    }

    @Override
    public String getSystemServiceName(Class<?> serviceClass) {
        String className = serviceClass.getName();
        if ("android.view.inputmethod.InputMethodManager".equals(className)) {
            return INPUT_METHOD_SERVICE;
        }
        if ("android.view.WindowManager".equals(className)) {
            return WINDOW_SERVICE;
        }
        if ("android.view.LayoutInflater".equals(className)) {
            return LAYOUT_INFLATER_SERVICE;
        }
        if ("android.view.accessibility.AccessibilityManager".equals(className)) {
            return ACCESSIBILITY_SERVICE;
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
        return packageName;
    }

    // ContextWrapper's implementation dereferences the null base used by
    // this detached context. Android's ViewRootImpl uses this identity while
    // constructing AttachInfo, so expose the same package-owned value.
    public String getBasePackageName() {
        return packageName;
    }

    // ProbeContext intentionally has no host ContextImpl backing object.  The
    // real Android ContextImpl nevertheless exposes the application
    // PathClassLoader, and LayoutInflater uses Context.getClassLoader() while
    // constructing custom views from an APK layout.  Return the owner thread's
    // installed app loader instead of delegating through the null ContextWrapper
    // base used by this detached Darwin context.
    @Override
    public ClassLoader getClassLoader() {
        // ContextImpl on Android returns the application PathClassLoader.  In
        // the detached launcher the support DEX and APK DEX are registered
        // together, but the support class can retain the loader that first
        // defined it.  Prefer the UI thread's installed application loader so
        // LayoutInflater resolves custom APK views (CalculatorEditText, etc.)
        // from the same loader as the Activity itself.
        ClassLoader application = applicationClassLoader;
        if (application != null) {
            return application;
        }
        ClassLoader current = Thread.currentThread().getContextClassLoader();
        return current != null ? current : classLoader;
    }

    /** Installs the Activity's complete APK+support PathClassLoader. */
    public static void installApplicationClassLoader(ClassLoader loader) {
        applicationClassLoader = loader;
    }

    @Override
    public String getOpPackageName() {
        return getPackageName();
    }

    @Override
    public boolean isRestricted() {
        return false;
    }

    // Hidden Context hook used while TextView resolves framework text
    // appearances. The trusted framework-res APK is the only resource owner
    // installed by this probe.
    public boolean canLoadUnsafeResources() {
        return true;
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
