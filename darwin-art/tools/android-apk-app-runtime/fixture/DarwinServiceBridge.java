package dev.darwinart.simple;

import android.os.Binder;
import android.graphics.Rect;
import android.util.Log;
import java.util.ArrayList;
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

    private DarwinServiceBridge() {}

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
                    (proxy, method, args) -> defaultValue(method.getReturnType()));
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
                            clipboardServiceValue));
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

        ManagerHandler(Object displayService, Object activityManagerService,
                Object activityService,
                Object inputMethodService, Object inputService, Object windowService,
                Object accessibilityService, Object sensitiveContentService,
                Object contentService, Object notificationService,
                Object voiceInteractionService, Object searchService,
                Object clipboardService) {
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
                        (proxy, method, args) -> defaultValue(method.getReturnType()));
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
