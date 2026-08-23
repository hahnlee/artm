package dev.darwinart.simple;

import android.os.Binder;
import android.util.Log;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.lang.reflect.Constructor;

/** Minimal in-process display service used by Choreographer on the host. */
final class DarwinServiceBridge {
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
            Object inputService = Proxy.newProxyInstance(
                    inputInterface.getClassLoader(),
                    new Class<?>[] {inputInterface},
                    (proxy, method, args) -> defaultValue(method.getReturnType()));
            attach(inputBinder, inputService, "android.hardware.input.IInputManager");

            Binder windowBinder = new Binder();
            Class<?> windowInterface = Class.forName("android.view.IWindowManager");
            Object windowService = Proxy.newProxyInstance(
                    windowInterface.getClassLoader(),
                    new Class<?>[] {windowInterface},
                    (proxy, method, args) -> defaultValue(method.getReturnType()));
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

            Class<?> metadataClass = Class.forName("android.os.ServiceWithMetadata");
            Class<?> serviceClass = Class.forName("android.os.Service");
            Object displayServiceValue = serviceValue(
                    metadataClass, serviceClass, displayBinder);
            Object activityServiceValue = serviceValue(
                    metadataClass, serviceClass, activityBinder);
            Object inputMethodServiceValue = serviceValue(
                    metadataClass, serviceClass, inputMethodBinder);
            Object inputServiceValue = serviceValue(metadataClass, serviceClass, inputBinder);
            Object windowServiceValue = serviceValue(
                    metadataClass, serviceClass, windowBinder);
            Object accessibilityServiceValue = serviceValue(
                    metadataClass, serviceClass, accessibilityBinder);
            Object sensitiveContentServiceValue = serviceValue(
                    metadataClass, serviceClass, sensitiveContentBinder);

            Class<?> managerInterface = Class.forName("android.os.IServiceManager");
            Object manager = Proxy.newProxyInstance(
                    managerInterface.getClassLoader(),
                    new Class<?>[] {managerInterface},
                    new ManagerHandler(displayServiceValue, activityServiceValue,
                            inputMethodServiceValue, inputServiceValue, windowServiceValue,
                            accessibilityServiceValue, sensitiveContentServiceValue));
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
        private final Object inputMethodService;
        private final Object inputService;
        private final Object windowService;
        private final Object accessibilityService;
        private final Object sensitiveContentService;

        ManagerHandler(Object displayService, Object activityService,
                Object inputMethodService, Object inputService, Object windowService,
                Object accessibilityService, Object sensitiveContentService) {
            this.displayService = displayService;
            this.activityService = activityService;
            this.inputMethodService = inputMethodService;
            this.inputService = inputService;
            this.windowService = windowService;
            this.accessibilityService = accessibilityService;
            this.sensitiveContentService = sensitiveContentService;
        }

        @Override
        public Object invoke(Object proxy, Method method, Object[] args) {
            if ((method.getName().equals("getService2")
                    || method.getName().equals("checkService2"))
                    && args != null && args.length > 0
                    && args[0] instanceof String) {
                if ("display".equals(args[0])) return displayService;
                if ("activity".equals(args[0]) || "activity_task".equals(args[0])) {
                    return activityService;
                }
                if ("input_method".equals(args[0])) return inputMethodService;
                if ("input".equals(args[0])) return inputService;
                if ("window".equals(args[0])) return windowService;
                if ("accessibility".equals(args[0])) return accessibilityService;
                if ("sensitive_content_protection_service".equals(args[0])) {
                    return sensitiveContentService;
                }
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
                put(infoClass, info, "logicalWidth", 360);
                put(infoClass, info, "logicalHeight", 640);
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
