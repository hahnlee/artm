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

            Class<?> metadataClass = Class.forName("android.os.ServiceWithMetadata");
            Object metadata = metadataClass.getDeclaredConstructor().newInstance();
            Field service = metadataClass.getDeclaredField("service");
            service.setAccessible(true);
            service.set(metadata, displayBinder);
            Field lazy = metadataClass.getDeclaredField("isLazyService");
            lazy.setAccessible(true);
            lazy.setBoolean(metadata, false);
            Class<?> serviceClass = Class.forName("android.os.Service");
            Constructor<?> serviceCtor = serviceClass.getDeclaredConstructor(
                    int.class, Object.class);
            serviceCtor.setAccessible(true);
            // Service's tagged union uses tag 0 for ServiceWithMetadata and
            // tag 1 for the raw accessor form.
            Object displayServiceValue = serviceCtor.newInstance(0, metadata);

            Class<?> managerInterface = Class.forName("android.os.IServiceManager");
            Object manager = Proxy.newProxyInstance(
                    managerInterface.getClassLoader(),
                    new Class<?>[] {managerInterface},
                    new ManagerHandler(displayServiceValue));
            Binder context = new Binder();
            attach(context, manager, "android.os.IServiceManager");
            return context;
        } catch (Throwable ignored) {
            Log.e("DarwinServiceBridge", "could not install in-process display service", ignored);
            return new Binder();
        }
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

        ManagerHandler(Object displayService) {
            this.displayService = displayService;
        }

        @Override
        public Object invoke(Object proxy, Method method, Object[] args) {
            if ((method.getName().equals("getService2")
                    || method.getName().equals("checkService2"))
                    && args != null && args.length > 0
                    && "display".equals(args[0])) {
                return displayService;
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
