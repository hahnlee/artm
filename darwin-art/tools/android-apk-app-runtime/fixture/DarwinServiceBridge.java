package dev.darwinart.simple;

import android.os.Binder;
import android.os.Handler;
import android.os.Looper;
import android.app.Activity;
import android.app.Application;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.graphics.Rect;
import android.util.Log;
import android.view.Display;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import dev.darwinart.probe.ProbeHostDocumentProvider;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Set;
import java.util.WeakHashMap;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.lang.reflect.Constructor;

/** Minimal in-process display service used by Choreographer on the host. */
final class DarwinServiceBridge {
    private static final int DISPLAY_SCALE =
            "2".equals(System.getenv("DARWIN_ART_WINDOW_SCALE")) ? 2 : 1;
    private static final int DISPLAY_WIDTH = 360 * DISPLAY_SCALE;
    private static final int DISPLAY_HEIGHT = 640 * DISPLAY_SCALE;
    private static final int DISPLAY_DENSITY_DPI = 160 * DISPLAY_SCALE;
    // ActivityTaskManager transactions originate outside the app main queue
    // on Android and must cross Choreographer's traversal sync barriers.
    private static final Handler MAIN_HANDLER = Handler.createAsync(Looper.getMainLooper());
    private static final Set<SurfaceView> HOST_SURFACES =
            Collections.newSetFromMap(new WeakHashMap<SurfaceView, Boolean>());
    private static final ArrayList<ActivityRecord> ACTIVITY_STACK = new ArrayList<>();
    private static volatile Activity currentActivity;

    private static final class ActivityRecord {
        final Activity activity;
        final android.os.IBinder token;
        final int requestCode;

        ActivityRecord(Activity activity, android.os.IBinder token, int requestCode) {
            this.activity = activity;
            this.token = token;
            this.requestCode = requestCode;
        }
    }

    private DarwinServiceBridge() {}

    /** Installs the Activity already launched by the native process bootstrap. */
    static void installInitialActivity(Activity activity) {
        currentActivity = activity;
        ACTIVITY_STACK.clear();
        ACTIVITY_STACK.add(new ActivityRecord(activity, activityToken(activity), -1));
        scheduleHostSurfaceScans(activity, activity.getWindow().getDecorView(), 300);
    }

    private static void scheduleHostSurfaceScans(
            Activity owner, View root, int remaining) {
        if (remaining <= 0) return;
        MAIN_HANDLER.postDelayed(() -> {
            if (currentActivity != owner) return;
            activateHostSurfaces(root);
            scheduleHostSurfaceScans(owner, root, remaining - 1);
        }, 16L);
    }

    /**
     * SurfaceFlinger normally announces a BufferQueue-backed Surface after a
     * ViewRoot traversal. The Darwin compositor owns that boundary, so it
     * announces an ANGLE pbuffer to every attached SurfaceView through the
     * public SurfaceHolder callback contract. The APK continues to own its
     * GLSurfaceView renderer and GL thread; no app class or listener is
     * special-cased here.
     */
    private static void activateHostSurfaces(View view) {
        if (view instanceof SurfaceView) {
            SurfaceView surfaceView = (SurfaceView) view;
            if (HOST_SURFACES.add(surfaceView)) {
                SurfaceHolder holder = surfaceView.getHolder();
                int[] location = new int[2];
                surfaceView.getLocationInWindow(location);
                nativeConfigureHostSurface(location[0], location[1],
                        Math.max(1, surfaceView.getWidth()),
                        Math.max(1, surfaceView.getHeight()));
                Log.i("DarwinServiceBridge", "activating host SurfaceView "
                        + surfaceView.getClass().getName() + " callback="
                        + (surfaceView instanceof SurfaceHolder.Callback)
                        + " size=" + surfaceView.getWidth() + "x"
                        + surfaceView.getHeight());
                if (surfaceView instanceof SurfaceHolder.Callback) {
                    SurfaceHolder.Callback callback =
                            (SurfaceHolder.Callback) surfaceView;
                    callback.surfaceCreated(holder);
                    callback.surfaceChanged(holder, 1,
                            Math.max(1, surfaceView.getWidth()),
                            Math.max(1, surfaceView.getHeight()));
                }
            }
        }
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            for (int index = 0; index < group.getChildCount(); ++index) {
                activateHostSurfaces(group.getChildAt(index));
            }
        }
    }

    /** Called by the host compositor after each Android main-message drain. */
    static void activateCurrentHostSurfaces() {
        Activity activity = currentActivity;
        if (activity != null) {
            activateHostSurfaces(activity.getWindow().getDecorView());
        }
    }

    private static void deactivateHostSurfaces(View view) {
        if (view instanceof SurfaceView) {
            SurfaceView surfaceView = (SurfaceView) view;
            if (HOST_SURFACES.remove(surfaceView)
                    && surfaceView instanceof SurfaceHolder.Callback) {
                ((SurfaceHolder.Callback) surfaceView).surfaceDestroyed(
                        surfaceView.getHolder());
            }
        }
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            for (int index = 0; index < group.getChildCount(); ++index) {
                deactivateHostSurfaces(group.getChildAt(index));
            }
        }
    }

    /** Rebinds the Metal/HWUI owner after an Android-owned Activity launch. */
    private static native boolean nativeInstallActivity(
            Activity activity, View decorView);
    private static native String nativeChooseDocument(String mimeType);
    private static native String[] nativeChooseSaveDocument(
            String mimeType, String suggestedName);
    private static native void nativeConfigureHostSurface(
            int x, int y, int width, int height);

    static Binder createContextBinder() {
        try {
            Binder displayBinder = new Binder();
            Class<?> displayInterface = Class.forName("android.hardware.display.IDisplayManager");
            Object displayService = Proxy.newProxyInstance(
                    displayInterface.getClassLoader(),
                    new Class<?>[] {displayInterface},
                    new DisplayHandler());
            attach(displayBinder, displayService, "android.hardware.display.IDisplayManager");

            // Calculator's Activity.onCreate reports its task description through
            // ActivityClient -> ActivityTaskManager.  A detached host has no
            // system_server, so expose a local no-op controller with the same
            // Android interface instead of returning a null ServiceWithMetadata.
            Binder activityBinder = new Binder();
            Class<?> activityTaskInterface =
                    Class.forName("android.app.IActivityTaskManager");
            Object activityTaskService = Proxy.newProxyInstance(
                    activityTaskInterface.getClassLoader(),
                    new Class<?>[] {activityTaskInterface},
                    new ActivityTaskHandler());
            attach(activityBinder, activityTaskService,
                    "android.app.IActivityTaskManager");

            Binder activityManagerBinder = new Binder();
            Class<?> activityManagerInterface = Class.forName("android.app.IActivityManager");
            Object activityManagerService = Proxy.newProxyInstance(
                    activityManagerInterface.getClassLoader(),
                    new Class<?>[] {activityManagerInterface},
                    new ActivityManagerHandler());
            attach(activityManagerBinder, activityManagerService, "android.app.IActivityManager");

            // ViewRootImpl construction also obtains InputMethodManager.  A
            // real app window receives this binder from system_server; the
            // host has no server, so provide the same typed interface with
            // conservative no-op results rather than a null ServiceWithMetadata.
            Binder inputMethodBinder = new Binder();
            Class<?> inputMethodInterface =
                    Class.forName("com.android.internal.view.IInputMethodManager");
            Object inputMethodService = Proxy.newProxyInstance(
                    inputMethodInterface.getClassLoader(),
                    new Class<?>[] {inputMethodInterface},
                    (proxy, method, args) -> defaultValue(method.getReturnType()));
            attach(inputMethodBinder, inputMethodService,
                    "com.android.internal.view.IInputMethodManager");

            // ViewGroup's native MotionEvent path creates a VelocityTracker.
            // Android obtains its device metadata through the input service;
            // provide a typed, conservative in-process service so dispatch
            // does not fall back to a null system_server object.
            Binder inputBinder = new Binder();
            Class<?> inputInterface = Class.forName("android.hardware.input.IInputManager");
            Object hostKeyboard = createKeyboard(
                    1, "Darwin host keyboard", "darwin-art-host-keyboard", true, 4);
            Object virtualKeyboard = createKeyboard(
                    -1, "Android virtual keyboard", "darwin-art-virtual-keyboard", false, 5);
            Object inputService = Proxy.newProxyInstance(
                    inputInterface.getClassLoader(),
                    new Class<?>[] {inputInterface},
                    (proxy, method, args) -> {
                        if ("getInputDeviceIds".equals(method.getName())) {
                            // Android always keeps VIRTUAL_KEYBOARD (-1) as a
                            // KeyCharacterMap fallback even when a physical
                            // keyboard is connected. Host NSEvents use id 1;
                            // -1 only satisfies framework default-map lookup.
                            return new int[] {1, -1};
                        }
                        if ("getInputDevice".equals(method.getName())) {
                            int id = ((Integer) args[0]).intValue();
                            return id == -1 ? virtualKeyboard : hostKeyboard;
                        }
                        return defaultValue(method.getReturnType());
                    });
            attach(inputBinder, inputService, "android.hardware.input.IInputManager");

            Binder windowBinder = new Binder();
            Class<?> windowInterface = Class.forName("android.view.IWindowManager");
            Object windowService = Proxy.newProxyInstance(
                    windowInterface.getClassLoader(),
                    new Class<?>[] {windowInterface},
                    new WindowManagerHandler());
            attach(windowBinder, windowService, "android.view.IWindowManager");

            // ViewRootImpl asks AccessibilityManager for the high-contrast
            // setting while it builds AttachInfo.  On Android this is backed
            // by the system_server accessibility service.  The Darwin host
            // has no system_server, so install the typed local service rather
            // than allowing ServiceManager.getService("accessibility") to
            // return null during ViewRootImpl construction.
            Binder accessibilityBinder = new Binder();
            Class<?> accessibilityInterface = Class.forName(
                    "android.view.accessibility.IAccessibilityManager");
            Object accessibilityService = Proxy.newProxyInstance(
                    accessibilityInterface.getClassLoader(),
                    new Class<?>[] {accessibilityInterface},
                    (proxy, method, args) -> defaultValue(method.getReturnType()));
            attach(accessibilityBinder, accessibilityService,
                    "android.view.accessibility.IAccessibilityManager");

            Binder sensitiveContentBinder = new Binder();
            Class<?> sensitiveContentInterface = Class.forName(
                    "android.view.ISensitiveContentProtectionManager");
            Object sensitiveContentService = Proxy.newProxyInstance(
                    sensitiveContentInterface.getClassLoader(),
                    new Class<?>[] {sensitiveContentInterface},
                    (proxy, method, args) -> defaultValue(method.getReturnType()));
            attach(sensitiveContentBinder, sensitiveContentService,
                    "android.view.ISensitiveContentProtectionManager");

            // ContentResolver registers observers through IContentService.
            // Keep the framework call path intact even though this detached
            // process has no system_server or cross-process providers.
            Binder contentBinder = new Binder();
            Class<?> contentInterface = Class.forName("android.content.IContentService");
            Object contentService = Proxy.newProxyInstance(
                    contentInterface.getClassLoader(),
                    new Class<?>[] {contentInterface},
                    (proxy, method, args) -> defaultValue(method.getReturnType()));
            attach(contentBinder, contentService, "android.content.IContentService");

            Binder notificationBinder = new Binder();
            Class<?> notificationInterface = Class.forName(
                    "android.app.INotificationManager");
            Object notificationService = Proxy.newProxyInstance(
                    notificationInterface.getClassLoader(),
                    new Class<?>[] {notificationInterface},
                    (proxy, method, args) -> defaultValue(method.getReturnType()));
            attach(notificationBinder, notificationService,
                    "android.app.INotificationManager");

            Binder voiceInteractionBinder = new Binder();
            Class<?> voiceInteractionInterface = Class.forName(
                    "com.android.internal.app.IVoiceInteractionManagerService");
            Object voiceInteractionService = Proxy.newProxyInstance(
                    voiceInteractionInterface.getClassLoader(),
                    new Class<?>[] {voiceInteractionInterface},
                    (proxy, method, args) -> defaultValue(method.getReturnType()));
            attach(voiceInteractionBinder, voiceInteractionService,
                    "com.android.internal.app.IVoiceInteractionManagerService");

            Binder searchBinder = new Binder();
            Class<?> searchInterface = Class.forName("android.app.ISearchManager");
            Object searchService = Proxy.newProxyInstance(
                    searchInterface.getClassLoader(), new Class<?>[] {searchInterface},
                    Proxy.getInvocationHandler(activityManagerService));
            attach(searchBinder, searchService, "android.app.ISearchManager");

            Binder clipboardBinder = new Binder();
            Class<?> clipboardInterface = Class.forName("android.content.IClipboard");
            Object clipboardService = Proxy.newProxyInstance(
                    clipboardInterface.getClassLoader(),
                    new Class<?>[] {clipboardInterface},
                    Proxy.getInvocationHandler(activityManagerService));
            attach(clipboardBinder, clipboardService, "android.content.IClipboard");

            Class<?> metadataClass = Class.forName("android.os.ServiceWithMetadata");
            Class<?> serviceClass = Class.forName("android.os.Service");
            Object displayServiceValue = serviceValue(
                    metadataClass, serviceClass, displayBinder);
            Object activityServiceValue = serviceValue(
                    metadataClass, serviceClass, activityBinder);
            Object activityManagerServiceValue = serviceValue(
                    metadataClass, serviceClass, activityManagerBinder);
            Object inputMethodServiceValue = serviceValue(
                    metadataClass, serviceClass, inputMethodBinder);
            Object inputServiceValue = serviceValue(metadataClass, serviceClass, inputBinder);
            Object windowServiceValue = serviceValue(
                    metadataClass, serviceClass, windowBinder);
            Object accessibilityServiceValue = serviceValue(
                    metadataClass, serviceClass, accessibilityBinder);
            Object sensitiveContentServiceValue = serviceValue(
                    metadataClass, serviceClass, sensitiveContentBinder);
            Object contentServiceValue = serviceValue(
                    metadataClass, serviceClass, contentBinder);
            Object notificationServiceValue = serviceValue(
                    metadataClass, serviceClass, notificationBinder);
            Object voiceInteractionServiceValue = serviceValue(
                    metadataClass, serviceClass, voiceInteractionBinder);
            Object searchServiceValue = serviceValue(
                    metadataClass, serviceClass, searchBinder);
            Object clipboardServiceValue = serviceValue(
                    metadataClass, serviceClass, clipboardBinder);
            Object missingServiceValue = serviceValue(
                    metadataClass, serviceClass, null);

            Class<?> managerInterface = Class.forName("android.os.IServiceManager");
            Object manager = Proxy.newProxyInstance(
                    managerInterface.getClassLoader(),
                    new Class<?>[] {managerInterface},
                    new ManagerHandler(displayServiceValue, activityManagerServiceValue,
                            activityServiceValue,
                            inputMethodServiceValue, inputServiceValue, windowServiceValue,
                            accessibilityServiceValue, sensitiveContentServiceValue,
                            contentServiceValue, notificationServiceValue,
                            voiceInteractionServiceValue, searchServiceValue,
                            clipboardServiceValue, missingServiceValue));
            Binder context = new Binder();
            attach(context, manager, "android.os.IServiceManager");
            return context;
        } catch (Throwable ignored) {
            Log.e("DarwinServiceBridge", "could not install in-process display service", ignored);
            return new Binder();
        }
    }

    private static Object serviceValue(Class<?> metadataClass, Class<?> serviceClass,
            Binder binder) throws Exception {
        Object metadata = metadataClass.getDeclaredConstructor().newInstance();
        Field service = metadataClass.getDeclaredField("service");
        service.setAccessible(true);
        service.set(metadata, binder);
        Field lazy = metadataClass.getDeclaredField("isLazyService");
        lazy.setAccessible(true);
        lazy.setBoolean(metadata, false);
        Constructor<?> serviceCtor = serviceClass.getDeclaredConstructor(int.class, Object.class);
        serviceCtor.setAccessible(true);
        return serviceCtor.newInstance(0, metadata);
    }

    private static Object createKeyboard(int id, String name, String descriptor,
            boolean external, int keyboardType) throws Exception {
        Class<?> keyMapClass = Class.forName("android.view.KeyCharacterMap");
        Method emptyMap = keyMapClass.getMethod("obtainEmptyMap", int.class);
        Object keyMap = emptyMap.invoke(null, Integer.valueOf(id));
        Class<?> builderClass = Class.forName("android.view.InputDevice$Builder");
        Object builder = builderClass.getDeclaredConstructor().newInstance();
        builderClass.getMethod("setId", int.class).invoke(builder, Integer.valueOf(id));
        builderClass.getMethod("setGeneration", int.class)
                .invoke(builder, Integer.valueOf(1));
        builderClass.getMethod("setName", String.class)
                .invoke(builder, name);
        builderClass.getMethod("setDescriptor", String.class)
                .invoke(builder, descriptor);
        builderClass.getMethod("setExternal", boolean.class)
                .invoke(builder, Boolean.valueOf(external));
        builderClass.getMethod("setSources", int.class)
                .invoke(builder, Integer.valueOf(0x101));
        builderClass.getMethod("setKeyboardType", int.class)
                .invoke(builder, Integer.valueOf(keyboardType));
        builderClass.getMethod("setKeyCharacterMap", keyMapClass)
                .invoke(builder, keyMap);
        builderClass.getMethod("setEnabled", boolean.class)
                .invoke(builder, Boolean.TRUE);
        return builderClass.getMethod("build").invoke(builder);
    }

    private static void attach(Binder binder, Object owner, String descriptor)
            throws Exception {
        Class<?> iInterface = Class.forName("android.os.IInterface");
        Method attach = Binder.class.getDeclaredMethod(
                "attachInterface", iInterface, String.class);
        attach.setAccessible(true);
        attach.invoke(binder, owner, descriptor);
    }

    private static final class ManagerHandler implements InvocationHandler {
        private final Object displayService;
        private final Object activityService;
        private final Object activityManagerService;
        private final Object inputMethodService;
        private final Object inputService;
        private final Object windowService;
        private final Object accessibilityService;
        private final Object sensitiveContentService;
        private final Object contentService;
        private final Object notificationService;
        private final Object voiceInteractionService;
        private final Object searchService;
        private final Object clipboardService;
        private final Object missingService;

        ManagerHandler(Object displayService, Object activityManagerService,
                Object activityService,
                Object inputMethodService, Object inputService, Object windowService,
                Object accessibilityService, Object sensitiveContentService,
                Object contentService, Object notificationService,
                Object voiceInteractionService, Object searchService,
                Object clipboardService, Object missingService) {
            this.displayService = displayService;
            this.activityManagerService = activityManagerService;
            this.activityService = activityService;
            this.inputMethodService = inputMethodService;
            this.inputService = inputService;
            this.windowService = windowService;
            this.accessibilityService = accessibilityService;
            this.sensitiveContentService = sensitiveContentService;
            this.contentService = contentService;
            this.notificationService = notificationService;
            this.voiceInteractionService = voiceInteractionService;
            this.searchService = searchService;
            this.clipboardService = clipboardService;
            this.missingService = missingService;
        }

        @Override
        public Object invoke(Object proxy, Method method, Object[] args) {
            if ((method.getName().equals("getService2")
                    || method.getName().equals("checkService2"))
                    && args != null && args.length > 0
                    && args[0] instanceof String) {
                if ("display".equals(args[0])) return displayService;
                if ("activity".equals(args[0])) return activityManagerService;
                if ("activity_task".equals(args[0])) {
                    return activityService;
                }
                if ("input_method".equals(args[0])) return inputMethodService;
                if ("input".equals(args[0])) return inputService;
                if ("window".equals(args[0])) return windowService;
                if ("accessibility".equals(args[0])) return accessibilityService;
                if ("sensitive_content_protection_service".equals(args[0])) {
                    return sensitiveContentService;
                }
                if ("content".equals(args[0])) return contentService;
                if ("notification".equals(args[0])) return notificationService;
                if ("voiceinteraction".equals(args[0])) {
                    return voiceInteractionService;
                }
                if ("search".equals(args[0])) return searchService;
                if ("clipboard".equals(args[0])) return clipboardService;
                return missingService;
            }
            return defaultValue(method.getReturnType());
        }
    }

    private static final class ActivityTaskHandler implements InvocationHandler {
        private final Object controller;

        ActivityTaskHandler() {
            Object value = null;
            try {
                Class<?> controllerInterface =
                        Class.forName("android.app.IActivityClientController");
                value = Proxy.newProxyInstance(
                        controllerInterface.getClassLoader(),
                        new Class<?>[] {controllerInterface},
                        new ActivityClientHandler());
            } catch (Throwable ignored) {
                // A missing controller is handled as an ordinary unsupported
                // service; framework callers still receive safe defaults.
            }
            controller = value;
        }

        @Override
        public Object invoke(Object proxy, Method method, Object[] args) {
            if (method.getName().equals("getActivityClientController")) {
                return controller;
            }
            if (method.getName().startsWith("startActivity")) {
                Intent intent = findArgument(args, Intent.class);
                ComponentName component = intent == null ? null : intent.getComponent();
                Log.i("DarwinServiceBridge", "activity task " + method.getName()
                        + " action=" + (intent == null ? null : intent.getAction())
                        + " component=" + component);
                String packageName = System.getenv("DARWIN_ART_APK_APP_PACKAGE");
                if (component != null && packageName != null
                        && packageName.equals(component.getPackageName())) {
                    Intent launchIntent = new Intent(intent);
                    int requestCode = findRequestCode(method, args, intent);
                    MAIN_HANDLER.post(() -> launchLocalActivity(launchIntent, requestCode));
                    // ActivityManager.START_SUCCESS. The transition itself is
                    // asynchronous on Android as well, after the current
                    // lifecycle callback returns to the main Looper.
                    return Integer.valueOf(0);
                }
                if (isOpenDocumentIntent(intent)) {
                    int requestCode = findRequestCode(method, args, intent);
                    deliverOpenDocumentResult(intent, requestCode);
                    return Integer.valueOf(0);
                }
                if (isCreateDocumentIntent(intent)) {
                    int requestCode = findRequestCode(method, args, intent);
                    deliverCreateDocumentResult(intent, requestCode);
                    return Integer.valueOf(0);
                }
            }
            return defaultValue(method.getReturnType());
        }
    }

    private static final class ActivityClientHandler implements InvocationHandler {
        @Override
        public Object invoke(Object proxy, Method method, Object[] args) {
            if ("finishActivity".equals(method.getName())) {
                android.os.IBinder token = findArgument(args, android.os.IBinder.class);
                Intent resultData = findArgument(args, Intent.class);
                int resultCode = args != null && args.length > 1
                        && args[1] instanceof Integer
                        ? ((Integer) args[1]).intValue() : Activity.RESULT_CANCELED;
                MAIN_HANDLER.post(() -> finishLocalActivity(token, resultCode, resultData));
                return Boolean.TRUE;
            }
            return defaultValue(method.getReturnType());
        }
    }

    private static boolean isOpenDocumentIntent(Intent intent) {
        if (intent == null) return false;
        String action = intent.getAction();
        return Intent.ACTION_GET_CONTENT.equals(action)
                || Intent.ACTION_OPEN_DOCUMENT.equals(action)
                || Intent.ACTION_PICK.equals(action);
    }

    private static boolean isCreateDocumentIntent(Intent intent) {
        return intent != null
                && Intent.ACTION_CREATE_DOCUMENT.equals(intent.getAction());
    }

    private static int findRequestCode(Method method, Object[] args, Intent intent) {
        if (args == null) return -1;
        Class<?>[] parameters = method.getParameterTypes();
        boolean afterIntent = false;
        for (int index = 0; index < args.length && index < parameters.length; index++) {
            if (args[index] == intent) {
                afterIntent = true;
            } else if (afterIntent && parameters[index] == int.class
                    && args[index] instanceof Integer) {
                return ((Integer) args[index]).intValue();
            }
        }
        return -1;
    }

    private static void deliverOpenDocumentResult(Intent request, int requestCode) {
        Activity recipient = currentActivity;
        if (recipient == null) return;
        String stagedPath = nativeChooseDocument(request.getType());
        Intent result = null;
        int resultCode = Activity.RESULT_CANCELED;
        if (stagedPath != null) {
            try {
                android.net.Uri uri = ProbeHostDocumentProvider.registerImport(stagedPath);
                result = new Intent().setDataAndType(uri,
                        request.getType() == null ? "image/jpeg" : request.getType());
                result.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
                resultCode = Activity.RESULT_OK;
            } catch (Throwable error) {
                Log.e("DarwinServiceBridge", "could not publish selected document", error);
            }
        }
        final Intent deliveredResult = result;
        final int deliveredCode = resultCode;
        MAIN_HANDLER.post(() -> dispatchActivityResult(
                recipient, requestCode, deliveredCode, deliveredResult));
    }

    private static void deliverCreateDocumentResult(Intent request, int requestCode) {
        Activity recipient = currentActivity;
        if (recipient == null) return;
        String title = request.getStringExtra(Intent.EXTRA_TITLE);
        String[] paths = nativeChooseSaveDocument(request.getType(), title);
        Log.i("DarwinServiceBridge", "create document result request=" + requestCode
                + " paths=" + (paths == null ? 0 : paths.length));
        Intent result = null;
        int resultCode = Activity.RESULT_CANCELED;
        if (paths != null && paths.length == 2
                && paths[0] != null && paths[1] != null) {
            try {
                android.net.Uri uri = ProbeHostDocumentProvider.registerExport(
                        paths[0], paths[1]);
                result = new Intent().setDataAndType(uri,
                        request.getType() == null ? "image/jpeg" : request.getType());
                result.addFlags(Intent.FLAG_GRANT_WRITE_URI_PERMISSION
                        | Intent.FLAG_GRANT_READ_URI_PERMISSION);
                resultCode = Activity.RESULT_OK;
            } catch (Throwable error) {
                Log.e("DarwinServiceBridge", "could not publish export document", error);
            }
        }
        final Intent deliveredResult = result;
        final int deliveredCode = resultCode;
        MAIN_HANDLER.post(() -> {
            Log.i("DarwinServiceBridge", "delivering create document result request="
                    + requestCode + " code=" + deliveredCode);
            dispatchActivityResult(
                    recipient, requestCode, deliveredCode, deliveredResult);
        });
    }

    private static void dispatchActivityResult(Activity activity, int requestCode,
            int resultCode, Intent data) {
        try {
            Method dispatch = Activity.class.getDeclaredMethod("dispatchActivityResult",
                    String.class, int.class, int.class, Intent.class, String.class);
            dispatch.setAccessible(true);
            dispatch.invoke(activity, null, Integer.valueOf(requestCode),
                    Integer.valueOf(resultCode), data, "darwin-art document picker");
            Log.i("DarwinServiceBridge", "Activity result delivered request="
                    + requestCode + " code=" + resultCode);
        } catch (Throwable error) {
            Log.e("DarwinServiceBridge", "document Activity result delivery failed", error);
        }
    }

    private static <T> T findArgument(Object[] args, Class<T> type) {
        if (args == null) return null;
        for (Object value : args) {
            if (type.isInstance(value)) return type.cast(value);
        }
        return null;
    }

    /**
     * Runs the same-process portion of ActivityTaskManager/ActivityThread.
     * The compatibility host has no system_server, so the local task service
     * schedules this transaction on Android's main Looper and still lets
     * Activity.attach(), PhoneWindow, Instrumentation, and ViewRootImpl own
     * their normal framework responsibilities.
     */
    private static void launchLocalActivity(Intent intent, int requestCode) {
        Activity previous = currentActivity;
        ComponentName component = intent == null ? null : intent.getComponent();
        if (previous == null || component == null) return;
        try {
            ClassLoader loader = previous.getClassLoader();
            Class<?> targetClass = Class.forName(component.getClassName(), true, loader);
            Activity next = (Activity) targetClass.getDeclaredConstructor().newInstance();

            Class<?> activityClass = Activity.class;
            Object activityThread = getField(activityClass, previous, "mMainThread");
            Object instrumentation = getField(activityClass, previous, "mInstrumentation");
            Application application = previous.getApplication();
            Context baseContext = previous.getBaseContext();
            ActivityInfo info;
            try {
                info = previous.getPackageManager().getActivityInfo(component, 0);
            } catch (Throwable ignored) {
                info = new ActivityInfo((ActivityInfo) getField(
                        activityClass, previous, "mActivityInfo"));
                info.name = component.getClassName();
            }
            Configuration configuration = new Configuration(
                    previous.getResources().getConfiguration());
            Binder token = new Binder();

            Class<?> activityThreadClass = Class.forName("android.app.ActivityThread");
            Class<?> instrumentationClass = Class.forName("android.app.Instrumentation");
            Class<?> voiceInteractorClass = Class.forName(
                    "com.android.internal.app.IVoiceInteractor");
            Class<?> activityConfigCallbackClass = Class.forName(
                    "android.view.ViewRootImpl$ActivityConfigCallback");
            Class<?> nonConfigurationClass = Class.forName(
                    "android.app.Activity$NonConfigurationInstances");
            Method attach = activityClass.getDeclaredMethod("attach",
                    Context.class, activityThreadClass, instrumentationClass,
                    android.os.IBinder.class, int.class, Application.class,
                    Intent.class, ActivityInfo.class, CharSequence.class,
                    Activity.class, String.class, nonConfigurationClass,
                    Configuration.class, String.class, voiceInteractorClass,
                    Window.class, activityConfigCallbackClass,
                    android.os.IBinder.class, android.os.IBinder.class);
            attach.setAccessible(true);
            attach.invoke(next, baseContext, activityThread, instrumentation,
                    token, Integer.valueOf(1), application, intent, info,
                    previous.getTitle(), null, null, null, configuration,
                    previous.getPackageName(), null, null, null, token, token);

            // ActivityThread applies the resolved ActivityInfo theme before
            // Instrumentation calls onCreate(). Each Activity owns a fresh
            // Theme even though this process-local runtime shares one APK
            // Resources table across its task.
            Resources.Theme activityTheme = previous.getResources().newTheme();
            activityTheme.applyStyle(
                    android.R.style.Theme_Material_Light_NoActionBar, true);
            if (info.theme != 0) activityTheme.applyStyle(info.theme, true);
            Class<?> contextThemeWrapper = Class.forName(
                    "android.view.ContextThemeWrapper");
            Method setTheme = contextThemeWrapper.getDeclaredMethod(
                    "setTheme", Resources.Theme.class);
            setTheme.setAccessible(true);
            setTheme.invoke(next, activityTheme);

            Method performPause = activityClass.getDeclaredMethod("performPause");
            Method performCreate = activityClass.getDeclaredMethod(
                    "performCreate", android.os.Bundle.class);
            Method performStart = activityClass.getDeclaredMethod(
                    "performStart", String.class);
            Method performResume = activityClass.getDeclaredMethod(
                    "performResume", boolean.class, String.class);
            Method performStop = activityClass.getDeclaredMethod(
                    "performStop", boolean.class, String.class);
            performPause.setAccessible(true);
            performCreate.setAccessible(true);
            performStart.setAccessible(true);
            performResume.setAccessible(true);
            performStop.setAccessible(true);

            performPause.invoke(previous);
            performCreate.invoke(next, new Object[] {null});
            performStart.invoke(next, "darwin-art activity launch");

            Window window = next.getWindow();
            View decor = window.getDecorView();
            Object global = Class.forName("android.view.WindowManagerGlobal")
                    .getMethod("getInstance").invoke(null);
            Method addView = global.getClass().getDeclaredMethod("addView",
                    View.class, ViewGroup.LayoutParams.class, Display.class,
                    Window.class, int.class);
            addView.setAccessible(true);
            addView.invoke(global, decor, window.getAttributes(), next.getDisplay(),
                    window, Integer.valueOf(0));
            decor.layout(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);
            if (!nativeInstallActivity(next, decor)) {
                throw new IllegalStateException("native graphics Activity install failed");
            }
            currentActivity = next;
            ACTIVITY_STACK.add(new ActivityRecord(next, token, requestCode));
            performResume.invoke(next, Boolean.FALSE, "darwin-art activity launch");
            // Fragment transactions committed from Activity.onCreate() can
            // materialize their SurfaceViews only after performResume().
            // Queue the compositor announcement behind those transactions.
            scheduleHostSurfaceScans(next, decor, 300);
            performStop.invoke(previous, Boolean.FALSE, "darwin-art activity launch");
            deactivateHostSurfaces(previous.getWindow().getDecorView());

            try {
                Method removeView = global.getClass().getDeclaredMethod(
                        "removeView", View.class, boolean.class);
                removeView.setAccessible(true);
                removeView.invoke(global, previous.getWindow().getDecorView(), Boolean.TRUE);
            } catch (Throwable ignored) {
                // The old ViewRoot is already excluded from native input and
                // presentation ownership. Cleanup failure must not undo the
                // successfully committed foreground Activity transaction.
            }
            Log.i("DarwinServiceBridge", "launched local Activity " + component);
        } catch (Throwable error) {
            Log.e("DarwinServiceBridge", "local Activity launch failed " + component, error);
        }
    }

    /** Completes the local Activity transaction and restores its caller. */
    private static void finishLocalActivity(android.os.IBinder token, int resultCode,
            Intent resultData) {
        if (ACTIVITY_STACK.size() < 2) return;
        ActivityRecord finishing = ACTIVITY_STACK.get(ACTIVITY_STACK.size() - 1);
        if (token != null && token != finishing.token) return;
        ActivityRecord caller = ACTIVITY_STACK.get(ACTIVITY_STACK.size() - 2);
        try {
            Class<?> activityClass = Activity.class;
            Method performPause = activityClass.getDeclaredMethod("performPause");
            Method performStop = activityClass.getDeclaredMethod(
                    "performStop", boolean.class, String.class);
            Method performDestroy = activityClass.getDeclaredMethod("performDestroy");
            Method performRestart = activityClass.getDeclaredMethod(
                    "performRestart", boolean.class);
            Method performResume = activityClass.getDeclaredMethod(
                    "performResume", boolean.class, String.class);
            performPause.setAccessible(true);
            performStop.setAccessible(true);
            performDestroy.setAccessible(true);
            performRestart.setAccessible(true);
            performResume.setAccessible(true);

            performPause.invoke(finishing.activity);
            deactivateHostSurfaces(finishing.activity.getWindow().getDecorView());
            Object global = Class.forName("android.view.WindowManagerGlobal")
                    .getMethod("getInstance").invoke(null);
            Method removeView = global.getClass().getDeclaredMethod(
                    "removeView", View.class, boolean.class);
            removeView.setAccessible(true);
            removeView.invoke(global,
                    finishing.activity.getWindow().getDecorView(), Boolean.TRUE);
            performStop.invoke(finishing.activity, Boolean.FALSE,
                    "darwin-art activity finish");
            performDestroy.invoke(finishing.activity);
            ACTIVITY_STACK.remove(ACTIVITY_STACK.size() - 1);

            performRestart.invoke(caller.activity, Boolean.TRUE);
            Window window = caller.activity.getWindow();
            View decor = window.getDecorView();
            Method addView = global.getClass().getDeclaredMethod("addView",
                    View.class, ViewGroup.LayoutParams.class, Display.class,
                    Window.class, int.class);
            addView.setAccessible(true);
            addView.invoke(global, decor, window.getAttributes(),
                    caller.activity.getDisplay(), window, Integer.valueOf(0));
            decor.layout(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);
            if (!nativeInstallActivity(caller.activity, decor)) {
                throw new IllegalStateException(
                        "native graphics caller restore failed");
            }
            currentActivity = caller.activity;
            if (finishing.requestCode >= 0) {
                dispatchActivityResult(caller.activity, finishing.requestCode,
                        resultCode, resultData);
            }
            performResume.invoke(caller.activity, Boolean.FALSE,
                    "darwin-art activity finish");
            scheduleHostSurfaceScans(caller.activity, decor, 300);
            Log.i("DarwinServiceBridge", "finished local Activity "
                    + finishing.activity.getComponentName() + " -> "
                    + caller.activity.getComponentName());
        } catch (Throwable error) {
            Log.e("DarwinServiceBridge", "local Activity finish failed", error);
        }
    }

    private static android.os.IBinder activityToken(Activity activity) {
        try {
            return (android.os.IBinder) getField(Activity.class, activity, "mToken");
        } catch (Throwable error) {
            Log.e("DarwinServiceBridge", "could not read Activity token", error);
            return null;
        }
    }

    private static Object getField(Class<?> owner, Object target, String name)
            throws Exception {
        Field field = owner.getDeclaredField(name);
        field.setAccessible(true);
        return field.get(target);
    }

    /** Process-local ActivityManager state normally supplied by system_server. */
    private static final class ActivityManagerHandler implements InvocationHandler {
        @Override
        public Object invoke(Object proxy, Method method, Object[] args) throws Exception {
            if ("getRunningAppProcesses".equals(method.getName())) {
                Class<?> processInfoType = Class.forName(
                        "android.app.ActivityManager$RunningAppProcessInfo");
                Object processInfo = processInfoType.getDeclaredConstructor().newInstance();
                String packageName = System.getenv("DARWIN_ART_APK_APP_PACKAGE");
                if (packageName == null || packageName.isEmpty()) {
                    packageName = "dev.darwinart.probe";
                }
                processInfoType.getField("processName").set(processInfo, packageName);
                processInfoType.getField("pid").setInt(
                        processInfo, android.os.Process.myPid());
                processInfoType.getField("uid").setInt(
                        processInfo, android.os.Process.myUid());
                processInfoType.getField("importance").setInt(processInfo, 100);
                ArrayList<Object> processes = new ArrayList<>();
                processes.add(processInfo);
                return processes;
            }
            return defaultValue(method.getReturnType());
        }
    }

    private static final class DisplayHandler implements InvocationHandler {
        @Override
        public Object invoke(Object proxy, Method method, Object[] args) {
            if (method.getName().equals("getDisplayInfo")) {
                Log.i("DarwinServiceBridge", "display getDisplayInfo");
                return displayInfo();
            }
            return defaultValue(method.getReturnType());
        }

        private static Object displayInfo() {
            try {
                Class<?> infoClass = Class.forName("android.view.DisplayInfo");
                Constructor<?> constructor = infoClass.getDeclaredConstructor();
                constructor.setAccessible(true);
                Object info = constructor.newInstance();
                put(infoClass, info, "logicalWidth", DISPLAY_WIDTH);
                put(infoClass, info, "logicalHeight", DISPLAY_HEIGHT);
                put(infoClass, info, "appWidth", DISPLAY_WIDTH);
                put(infoClass, info, "appHeight", DISPLAY_HEIGHT);
                put(infoClass, info, "logicalDensityDpi", DISPLAY_DENSITY_DPI);
                put(infoClass, info, "physicalXDpi", (float) DISPLAY_DENSITY_DPI);
                put(infoClass, info, "physicalYDpi", (float) DISPLAY_DENSITY_DPI);
                // Android 16 derives DisplayInfo.getRefreshRate() from these
                // fields (the old single refreshRate field no longer exists).
                put(infoClass, info, "refreshRateOverride", 60.0f);
                put(infoClass, info, "renderFrameRate", 60.0f);
                return info;
            } catch (Throwable ignored) {
                Log.e("DarwinServiceBridge", "display info construction failed", ignored);
                return null;
            }
        }

        private static void put(Class<?> type, Object target, String name, Object value)
                throws Exception {
            Field field = type.getDeclaredField(name);
            field.setAccessible(true);
            field.set(target, value);
        }
    }

    /** Supplies the process-local IWindowSession used by popup ViewRoots. */
    private static final class WindowManagerHandler implements InvocationHandler {
        private final Object session;
        private final ArrayList<Object> serverInputChannels = new ArrayList<>();

        WindowManagerHandler() throws ClassNotFoundException {
            Class<?> sessionInterface = Class.forName("android.view.IWindowSession");
            session = Proxy.newProxyInstance(sessionInterface.getClassLoader(),
                    new Class<?>[] {sessionInterface}, this);
        }

        @Override
        public Object invoke(Object proxy, Method method, Object[] args) {
            if (System.getenv("DARWIN_ART_DEBUG_WINDOW_LAYERS") != null) {
                Log.i("DarwinServiceBridge", "window " + method.getName()
                        + " argc=" + (args == null ? 0 : args.length)
                        + " session=" + (proxy == session));
            }
            if (proxy != session && "openSession".equals(method.getName())) {
                return session;
            }
            if (proxy == session && method.getName().startsWith("addToDisplay")) {
                initializeAddOutputs(args);
                return Integer.valueOf(0);
            }
            if (proxy == session && "relayout".equals(method.getName())) {
                configureRelayout(args);
                // RELAYOUT_RES_FIRST_TIME: ViewRoot must complete its first
                // traversal even though CAMetalLayer owns the real surface.
                return Integer.valueOf(2);
            }
            return defaultValue(method.getReturnType());
        }

        private void initializeAddOutputs(Object[] args) {
            if (args == null) return;
            for (Object value : args) {
                if (value != null && value.getClass().getName().equals(
                        "android.view.WindowManager$LayoutParams")) {
                    normalizeLayoutParams(value);
                } else if (value instanceof Rect) {
                    ((Rect) value).set(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);
                } else if (value instanceof float[] && ((float[]) value).length > 0) {
                    ((float[]) value)[0] = 1.0f;
                } else if (value != null && value.getClass().getName().equals(
                        "android.view.InputChannel")) {
                    try {
                        Class<?> inputChannelClass = value.getClass();
                        Method openPair = inputChannelClass.getDeclaredMethod(
                                "openInputChannelPair", String.class);
                        openPair.setAccessible(true);
                        Object[] pair = (Object[]) openPair.invoke(null,
                                "darwin-art-window-" + serverInputChannels.size());
                        inputChannelClass.getMethod("copyTo", inputChannelClass)
                                .invoke(pair[0], value);
                        inputChannelClass.getMethod("dispose").invoke(pair[0]);
                        serverInputChannels.add(pair[1]);
                    } catch (Throwable error) {
                        Log.e("DarwinServiceBridge", "input channel setup failed", error);
                    }
                }
            }
        }

        private static void normalizeLayoutParams(Object attrs) {
            try {
                Field width = attrs.getClass().getField("width");
                Field height = attrs.getClass().getField("height");
                Field type = attrs.getClass().getField("type");
                int windowType = type.getInt(attrs);
                if (width.getInt(attrs) <= 0) width.setInt(attrs, DISPLAY_WIDTH);
                if (height.getInt(attrs) <= 0) {
                    height.setInt(attrs, windowType >= 1 && windowType < 1000
                            ? DISPLAY_HEIGHT : 120 * DISPLAY_SCALE);
                }
            } catch (Throwable error) {
                Log.e("DarwinServiceBridge", "window layout normalization failed", error);
            }
        }

        private static void configureRelayout(Object[] args) {
            if (args == null || args.length < 9 || args[8] == null) return;
            try {
                Object attrs = args[1];
                int requestedWidth = ((Integer) args[2]).intValue();
                int requestedHeight = ((Integer) args[3]).intValue();
                int x = intField(attrs, "x", 0);
                int y = intField(attrs, "y", 0);
                int layoutWidth = intField(attrs, "width", requestedWidth);
                int layoutHeight = intField(attrs, "height", requestedHeight);
                int width = requestedWidth > 0 && requestedWidth <= DISPLAY_WIDTH
                        ? requestedWidth
                        : (layoutWidth > 0 && layoutWidth <= DISPLAY_WIDTH
                                ? layoutWidth : DISPLAY_WIDTH);
                int height = requestedHeight > 0 && requestedHeight <= DISPLAY_HEIGHT
                        ? requestedHeight
                        : (layoutHeight > 0 && layoutHeight <= DISPLAY_HEIGHT
                                ? layoutHeight : 120 * DISPLAY_SCALE);
                width = Math.min(width, DISPLAY_WIDTH - Math.max(0, x));
                height = Math.min(height, DISPLAY_HEIGHT - Math.max(0, y));
                if (System.getenv("DARWIN_ART_DEBUG_WINDOW_LAYERS") != null) {
                    Log.i("DarwinServiceBridge", "window frame request="
                            + requestedWidth + "x" + requestedHeight
                            + " layout=" + layoutWidth + "x" + layoutHeight
                            + " output=" + width + "x" + height + " at=" + x + "," + y);
                }

                Object result = args[8];
                Field framesField = result.getClass().getField("frames");
                Object frames = framesField.get(result);
                setRectField(frames, "frame", x, y, x + width, y + height);
                setRectField(frames, "displayFrame", 0, 0,
                        DISPLAY_WIDTH, DISPLAY_HEIGHT);
                setRectField(frames, "parentFrame", 0, 0,
                        DISPLAY_WIDTH, DISPLAY_HEIGHT);
            } catch (Throwable error) {
                Log.e("DarwinServiceBridge", "window relayout output failed", error);
            }
        }

        private static int intField(Object value, String name, int fallback)
                throws Exception {
            if (value == null) return fallback;
            Field field = value.getClass().getField(name);
            return field.getInt(value);
        }

        private static void setRectField(Object value, String name,
                int left, int top, int right, int bottom) throws Exception {
            Field field = value.getClass().getField(name);
            ((Rect) field.get(value)).set(left, top, right, bottom);
        }
    }

    private static Object defaultValue(Class<?> type) {
        if (!type.isPrimitive()) return null;
        if (type == boolean.class) return false;
        if (type == byte.class) return (byte) 0;
        if (type == short.class) return (short) 0;
        if (type == char.class) return (char) 0;
        if (type == int.class) return 0;
        if (type == long.class) return 0L;
        if (type == float.class) return 0.0f;
        if (type == double.class) return 0.0d;
        return null;
    }
}
