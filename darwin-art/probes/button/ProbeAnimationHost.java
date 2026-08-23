package dev.darwinart.probe;

import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.ArrayList;
import android.content.Context;
import android.os.Binder;
import android.os.Handler;
import android.os.Looper;
import android.view.View;
import android.view.ViewGroup;

/** Creates the hidden RenderNode.AnimationHost without hidden compile stubs. */
public final class ProbeAnimationHost {
    private ProbeAnimationHost() {}
    private static Object sViewRootImpl;

    public static Object create(Class<?> host) {
        try {
            InvocationHandler handler = new InvocationHandler() {
                private final ArrayList<Object> running = new ArrayList<>();

                @Override
                public Object invoke(Object proxy, Method method, Object[] args) {
                    if ("isAttached".equals(method.getName())) {
                        return Boolean.TRUE;
                    }
                    if ("registerAnimatingRenderNode".equals(method.getName()) &&
                            args != null && args.length > 1 && args[1] != null &&
                            !running.contains(args[1])) {
                        // Keep animators reachable like ViewRootImpl; the native
                        // RenderNode owns and advances its native counterpart.
                        running.add(args[1]);
                    }
                    return null;
                }
            };
            return Proxy.newProxyInstance(host.getClassLoader(), new Class<?>[] {host}, handler);
        } catch (Throwable error) {
            error.printStackTrace();
            return null;
        }
    }

    /**
     * Mirrors the first attached ViewRoot traversal for support ViewPager.
     * The calculator APK is unchanged; this only supplies the missing window
     * attachment preparation in the host-side probe hierarchy.
     */
    public static void prepareViewPagers(Object root) {
        if (!(root instanceof View)) {
            return;
        }
        Class<?> type = root.getClass();
        while (type != null) {
            try {
                Method populate = type.getDeclaredMethod("populate");
                populate.setAccessible(true);
                populate.invoke(root);
                break;
            } catch (NoSuchMethodException ignored) {
                type = type.getSuperclass();
            } catch (Throwable error) {
                // A pager may legitimately defer population until its adapter
                // is ready; leave the normal Android draw path to decide.
                break;
            }
        }
        // populate() intentionally defers when getWindowToken() is null.  A
        // detached host still needs the same child-order cache for a faithful
        // first draw, so invoke the support ViewPager's private sorting hook
        // and seed its per-child indices exactly as populate() would.
        type = root.getClass();
        while (type != null) {
            try {
                Method sort = type.getDeclaredMethod("sortChildDrawingOrder");
                sort.setAccessible(true);
                sort.invoke(root);
                break;
            } catch (NoSuchMethodException ignored) {
                type = type.getSuperclass();
            } catch (Throwable error) {
                error.printStackTrace();
                break;
            }
        }
        if (root instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) root;
            for (int i = 0; i < group.getChildCount(); ++i) {
                View child = group.getChildAt(i);
                try {
                    Object params = child.getLayoutParams();
                    if (params != null) {
                        Field index = params.getClass().getDeclaredField("childIndex");
                        index.setAccessible(true);
                        index.setInt(params, i);
                    }
                } catch (Throwable ignored) {
                    // Non-ViewPager children have ordinary LayoutParams.
                }
                prepareViewPagers(child);
            }
        }
    }

    /** Attach the real hierarchy bookkeeping without creating a second window.
     * The Metal surface remains owned by the host; this only supplies the
     * AttachInfo that Android View.draw() normally receives from ViewRootImpl.
     */
    public static boolean attachHardwareHierarchy(Object root, Object contextObject) {
        if (sViewRootImpl != null) return true;
        if (!(root instanceof View)) return false;
        try {
            Context context = contextObject instanceof Context
                    ? (Context) contextObject : ((View) root).getContext();
            if (context == null) {
                Class<?> probeContext = Class.forName("dev.darwinart.probe.ProbeContext");
                Constructor<?> probeConstructor = probeContext.getConstructor(
                        Class.forName("android.content.res.Resources"),
                        Class.forName("android.content.pm.PackageManager"),
                        String.class);
                context = (Context) probeConstructor.newInstance(
                        ((View) root).getResources(), null, "com.android.calculator2");
            }
            if (context == null) return false;
            // Build the same AttachInfo object directly: View's hardware
            // checks, invalidation, and RenderNode recording observe the
            // normal Android fields, while the host's CAMetalLayer remains
            // the sole window owner.
            Class<?> displayType = Class.forName("android.view.Display");
            Class<?> iWindowSession = Class.forName("android.view.IWindowSession");
            Class<?> iWindow = Class.forName("android.view.IWindow");
            InvocationHandler windowHandler = (proxy, method, args) -> {
                if ("asBinder".equals(method.getName())) return new Binder();
                return defaultValue(method.getReturnType());
            };
            Object window = Proxy.newProxyInstance(
                    iWindow.getClassLoader(), new Class<?>[] {iWindow}, windowHandler);
            Object session = Proxy.newProxyInstance(
                    iWindowSession.getClassLoader(), new Class<?>[] {iWindowSession},
                    windowHandler);
            Class<?> attachType = Class.forName("android.view.View$AttachInfo");
            Class<?> callbacks = Class.forName("android.view.View$AttachInfo$Callbacks");
            // The real ViewRootImpl owns the invalidation/choreographer bridge
            // used by TextView.append() and postInvalidateOnAnimation().  The
            // detached host still creates it with the normal Android
            // constructor; only the WindowManager add/traversal transaction
            // remains omitted because CAMetalLayer is the window owner.
            Class<?> viewRootType = Class.forName("android.view.ViewRootImpl");
            Class<?> displayGlobalType = Class.forName(
                    "android.hardware.display.DisplayManagerGlobal");
            Method displayInstance = displayGlobalType.getDeclaredMethod("getInstance");
            displayInstance.setAccessible(true);
            Object displayGlobal = displayInstance.invoke(null);
            Method realDisplay = displayGlobalType.getDeclaredMethod(
                    "getRealDisplay", int.class);
            realDisplay.setAccessible(true);
            Object display = realDisplay.invoke(displayGlobal, Integer.valueOf(0));
            if (display == null) return false;
            Constructor<?> viewRootConstructor = viewRootType.getDeclaredConstructor(
                    Context.class, displayType);
            viewRootConstructor.setAccessible(true);
            Object viewRoot = viewRootConstructor.newInstance(context, display);
            Constructor<?> attachConstructor = attachType.getDeclaredConstructor(
                    iWindowSession, iWindow, displayType,
                    viewRootType, Handler.class,
                    callbacks, Context.class);
            attachConstructor.setAccessible(true);
            Object attachInfo = attachConstructor.newInstance(
                    session, window, display, viewRoot,
                    new Handler(Looper.getMainLooper()), null, context);
            Field hardwareField = attachInfo.getClass().getDeclaredField(
                    "mHardwareAccelerated");
            hardwareField.setAccessible(true);
            hardwareField.setBoolean(attachInfo, true);
            Field requestedField = attachInfo.getClass().getDeclaredField(
                    "mHardwareAccelerationRequested");
            requestedField.setAccessible(true);
            requestedField.setBoolean(attachInfo, true);
            // Do not call dispatchAttachedToWindow here. That method assumes
            // a live ViewRootImpl (IME focus, window callbacks and traversal
            // callbacks) and is not safe for this windowless Metal owner. The
            // detached GPU traversal only needs the same AttachInfo pointer so
            // ViewGroup.dispatchDraw and hardware canvas checks retain their
            // normal Android decisions. Propagate it explicitly through the
            // hierarchy; no Binder/window lifecycle is synthesized.
            Field attachField = View.class.getDeclaredField("mAttachInfo");
            attachField.setAccessible(true);
            installAttachInfo((View) root, attachField, attachInfo);
            sViewRootImpl = attachInfo;
            return true;
        } catch (Throwable error) {
            error.printStackTrace();
            return false;
        }
    }

    private static Object defaultValue(Class<?> type) {
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

    private static void clearFocusTree(View view) {
        view.clearFocus();
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            for (int i = 0; i < group.getChildCount(); ++i) {
                clearFocusTree(group.getChildAt(i));
            }
        }
    }

    private static void installAttachInfo(View view, Field attachField,
            Object attachInfo) throws IllegalAccessException {
        if (view == null) return;
        attachField.set(view, attachInfo);
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            for (int i = 0; i < group.getChildCount(); ++i) {
                installAttachInfo(group.getChildAt(i), attachField, attachInfo);
            }
        }
    }
}
