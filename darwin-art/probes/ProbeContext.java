package dev.darwinart.probe;

import android.content.AttributionSource;
import android.content.BroadcastReceiver;
import android.content.ContentResolver;
import android.content.ContentCaptureOptions;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.pm.ProbeShortcutManager;
import android.content.pm.ShortcutManager;
import android.content.res.Resources;
import android.content.res.Configuration;
import android.os.Looper;
import android.os.Handler;
import android.os.Process;
import android.os.ProbeUserManager;
import android.os.UserHandle;
import android.os.UserManager;
import android.media.ProbeAudioManager;
import android.view.autofill.AutofillManager;
import android.view.Display;

import java.lang.reflect.Constructor;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.io.File;
import java.util.concurrent.Executor;

public final class ProbeContext extends ContextWrapper {
    private final ApplicationInfo applicationInfo;
    private final AttributionSource attributionSource;
    private final ContentResolver contentResolver;
    private final Resources resources;
    private final Resources.Theme theme;
    private final PackageManager packageManager;
    private final SharedPreferences sharedPreferences;
    private final ShortcutManager shortcutManager;
    private final UserManager userManager;
    private final String packageName;
    private final ClassLoader classLoader;
    private volatile Display display;
    private static volatile ClassLoader applicationClassLoader;

    private static final class MainExecutor implements Executor {
        @Override
        public void execute(Runnable command) {
            if (command == null) throw new NullPointerException("command");
            if (Looper.myLooper() == Looper.getMainLooper()) {
                command.run();
            } else {
                new Handler(Looper.getMainLooper()).post(command);
            }
        }
    }

    private final Executor mainExecutor = new MainExecutor();

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
        sharedPreferences = new ProbeSharedPreferences();
        shortcutManager = new ProbeShortcutManager();
        userManager = new ProbeUserManager();
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

    /** Applies the manifest compatibility level before Application.onCreate(). */
    public void setTargetSdkVersion(int targetSdkVersion) {
        applicationInfo.targetSdkVersion = targetSdkVersion;
    }

    /** Installs the target-SDK compatibility delegate normally owned by ActivityThread. */
    public static void configureCompatibility(int targetSdkVersion) {
        try {
            Class<?> compatibility = Class.forName("android.compat.Compatibility");
            Class<?> delegate = Class.forName(
                    "android.compat.Compatibility$BehaviorChangeDelegate");
            Object callbacks = Proxy.newProxyInstance(
                    delegate.getClassLoader(), new Class<?>[] {delegate},
                    new CompatibilityHandler(targetSdkVersion));
            Method install = compatibility.getMethod(
                    "setBehaviorChangeDelegate", delegate);
            install.invoke(null, callbacks);
        } catch (ReflectiveOperationException error) {
            throw new IllegalStateException("Could not install compatibility callbacks", error);
        }
    }

    private static final class CompatibilityHandler implements InvocationHandler {
        private final int targetSdkVersion;

        CompatibilityHandler(int targetSdkVersion) {
            this.targetSdkVersion = targetSdkVersion;
        }

        @Override
        public Object invoke(Object proxy, Method method, Object[] args) {
            if ("isChangeEnabled".equals(method.getName())
                    && args != null && args.length == 1 && args[0] instanceof Long) {
                long changeId = (Long) args[0];
                if (changeId == 160794467L) return targetSdkVersion > 30;
                if (changeId == 236704164L) return targetSdkVersion >= 34;
                if (changeId == 320664730L) return targetSdkVersion > 34;
                return true;
            }
            return null;
        }
    }

    private static final class DefaultServiceHandler implements InvocationHandler {
        @Override
        public Object invoke(Object proxy, Method method, Object[] args) {
            Class<?> type = method.getReturnType();
            if (!type.isPrimitive()) return null;
            if (type == boolean.class) return Boolean.FALSE;
            if (type == byte.class) return Byte.valueOf((byte) 0);
            if (type == short.class) return Short.valueOf((short) 0);
            if (type == char.class) return Character.valueOf((char) 0);
            if (type == int.class) return Integer.valueOf(0);
            if (type == long.class) return Long.valueOf(0L);
            if (type == float.class) return Float.valueOf(0.0f);
            if (type == double.class) return Double.valueOf(0.0d);
            return null;
        }
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
    public Context createDeviceProtectedStorageContext() {
        return this;
    }

    @Override
    public Context createConfigurationContext(Configuration overrideConfiguration) {
        // The detached owner exposes one display and one Resources instance.
        // PhoneWindow asks its application context for an equivalent display
        // configuration while constructing DecorContext; the already-applied
        // resource configuration is authoritative for that display.
        return this;
    }

    @Override
    public Context createDisplayContext(Display requestedDisplay) {
        if (requestedDisplay == null || requestedDisplay.getDisplayId() != 0) {
            throw new IllegalArgumentException("Only the primary Android display is available");
        }
        return this;
    }

    @Override
    public boolean isDeviceProtectedStorage() {
        return true;
    }

    @Override
    public boolean moveSharedPreferencesFrom(Context sourceContext, String name) {
        return true;
    }

    @Override
    public SharedPreferences getSharedPreferences(String name, int mode) {
        return sharedPreferences;
    }

    private File appDataPath(String child) {
        String configured = System.getenv("DARWIN_ART_APK_APP_DATA_DIR");
        File root = new File(configured == null
                ? new File(System.getProperty("java.io.tmpdir"), "darwin-art-app-data")
                        .getPath()
                : configured);
        File result = child == null ? root : new File(root, child);
        result.mkdirs();
        return result;
    }

    @Override
    public File getDataDir() {
        return appDataPath(null);
    }

    @Override
    public File getFilesDir() {
        return appDataPath("files");
    }

    @Override
    public File getCacheDir() {
        return appDataPath("cache");
    }

    @Override
    public File getCodeCacheDir() {
        return appDataPath("code_cache");
    }

    @Override
    public File getNoBackupFilesDir() {
        return appDataPath("no_backup");
    }

    @Override
    public File getDatabasePath(String name) {
        File databases = appDataPath("databases");
        return new File(databases, name);
    }

    @Override
    public ContentResolver getContentResolver() {
        return contentResolver;
    }

    @Override
    public Intent registerReceiver(BroadcastReceiver receiver, IntentFilter filter) {
        // A detached app process has no ActivityManager broadcast registry.
        // Registration succeeds locally; no sticky broadcast is pending.
        return null;
    }

    public Intent registerReceiverAsUser(BroadcastReceiver receiver, UserHandle user,
            IntentFilter filter, String broadcastPermission, Handler scheduler) {
        return null;
    }

    @Override
    public void unregisterReceiver(BroadcastReceiver receiver) {}

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
    public Executor getMainExecutor() {
        return mainExecutor;
    }

    /** ContextImpl's display identity for PhoneWindow / DecorContext. */
    public Display getDisplayNoVerify() {
        Display current = display;
        if (current != null) return current;
        try {
            Class<?> globalType = Class.forName(
                    "android.hardware.display.DisplayManagerGlobal");
            Method getInstance = globalType.getDeclaredMethod("getInstance");
            getInstance.setAccessible(true);
            Object global = getInstance.invoke(null);
            Method getRealDisplay = globalType.getDeclaredMethod(
                    "getRealDisplay", int.class);
            getRealDisplay.setAccessible(true);
            current = (Display) getRealDisplay.invoke(global, Integer.valueOf(0));
            if (current == null) {
                throw new IllegalStateException("Primary Android display is unavailable");
            }
            display = current;
            return current;
        } catch (ReflectiveOperationException error) {
            throw new IllegalStateException("Could not resolve primary Android display", error);
        }
    }

    @Override
    public Display getDisplay() {
        return getDisplayNoVerify();
    }

    /** Hidden Context virtual used by ViewConfiguration on Android 16. */
    public int getDisplayId() {
        return 0;
    }

    /** Hidden ContextImpl hook used by DecorContext during window creation. */
    public void updateDisplay(int displayId) {
        if (displayId != 0) {
            throw new IllegalArgumentException("Only the primary Android display is available");
        }
        getDisplayNoVerify();
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
        if (ACCESSIBILITY_SERVICE.equals(name)) {
            try {
                Class<?> type = Class.forName(
                        "android.view.accessibility.AccessibilityManager");
                Method factory = type.getMethod("getInstance", Context.class);
                return factory.invoke(null, this);
            } catch (ReflectiveOperationException error) {
                throw new IllegalStateException(
                        "Could not construct accessibility manager", error);
            }
        }
        if (SHORTCUT_SERVICE.equals(name)) {
            return shortcutManager;
        }
        if (NOTIFICATION_SERVICE.equals(name)) {
            return construct("android.app.NotificationManager");
        }
        if (USER_SERVICE.equals(name)) {
            return userManager;
        }
        if (VIBRATOR_SERVICE.equals(name)) {
            return construct("android.os.SystemVibrator");
        }
        if (ALARM_SERVICE.equals(name)) {
            return constructProxyManager("android.app.AlarmManager",
                    "android.app.IAlarmManager");
        }
        if (AUDIO_SERVICE.equals(name)) {
            return new ProbeAudioManager(this);
        }
        if (CLIPBOARD_SERVICE.equals(name)) {
            try {
                Class<?> type = Class.forName("android.content.ClipboardManager");
                Constructor<?> constructor = type.getDeclaredConstructor(
                        Context.class, Handler.class);
                constructor.setAccessible(true);
                return constructor.newInstance(this, new Handler(Looper.getMainLooper()));
            } catch (ReflectiveOperationException error) {
                throw new IllegalStateException("Could not construct clipboard manager", error);
            }
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
        if ("android.content.pm.ShortcutManager".equals(className)) {
            return SHORTCUT_SERVICE;
        }
        if ("android.app.NotificationManager".equals(className)) {
            return NOTIFICATION_SERVICE;
        }
        if ("android.content.ClipboardManager".equals(className)) {
            return CLIPBOARD_SERVICE;
        }
        if ("android.os.UserManager".equals(className)) {
            return USER_SERVICE;
        }
        if ("android.os.Vibrator".equals(className)) {
            return VIBRATOR_SERVICE;
        }
        if ("android.app.AlarmManager".equals(className)) {
            return ALARM_SERVICE;
        }
        if ("android.media.AudioManager".equals(className)) {
            return AUDIO_SERVICE;
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
    public String getAttributionTag() {
        return null;
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

    // Android 16 associates Contexts with the default virtual device unless
    // a device-aware display context says otherwise. The Darwin host exposes
    // one display/device, so framework managers consistently use device 0.
    public int getDeviceId() {
        return 0;
    }

    // Hidden virtual Context hook used by NotificationManager.
    public UserHandle getUser() {
        return Process.myUserHandle();
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

    private Object constructProxyManager(String className, String interfaceName) {
        try {
            Class<?> interfaceType = Class.forName(interfaceName);
            Object service = Proxy.newProxyInstance(interfaceType.getClassLoader(),
                    new Class<?>[] {interfaceType}, new DefaultServiceHandler());
            Class<?> type = Class.forName(className);
            Constructor<?> constructor = type.getDeclaredConstructor(
                    interfaceType, Context.class);
            constructor.setAccessible(true);
            return constructor.newInstance(service, this);
        } catch (ReflectiveOperationException error) {
            throw new IllegalStateException("Could not construct " + className, error);
        }
    }

}
