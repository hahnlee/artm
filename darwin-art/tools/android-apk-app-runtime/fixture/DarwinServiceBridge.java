package dev.darwinart.simple;

import android.os.Binder;
import android.os.Bundle;
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
import android.view.Surface;
import android.view.SurfaceControl;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewParent;
import android.view.Window;
import dev.darwinart.probe.ProbeHostDocumentProvider;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Map;
import java.util.WeakHashMap;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.lang.reflect.Constructor;
import java.lang.reflect.Array;
import java.io.File;

/** Minimal in-process display service used by Choreographer on the host. */
public final class DarwinServiceBridge {
    private static final int DISPLAY_SCALE =
            "2".equals(System.getenv("DARWIN_ART_WINDOW_SCALE")) ? 2 : 1;
    private static volatile int DISPLAY_WIDTH = 360 * DISPLAY_SCALE;
    private static volatile int DISPLAY_HEIGHT = 640 * DISPLAY_SCALE;
    private static final int DISPLAY_DENSITY_DPI = 160 * DISPLAY_SCALE;
    // android.view.Display.TYPE_INTERNAL is a hidden framework constant. Keep
    // the platform ABI value here because android.jar intentionally omits it.
    private static final int DISPLAY_TYPE_INTERNAL = 1;
    // ActivityTaskManager transactions originate outside the app main queue
    // on Android and must cross Choreographer's traversal sync barriers.
    private static final Handler MAIN_HANDLER = Handler.createAsync(Looper.getMainLooper());
    private static final Map<SurfaceView, HostSurfaceState> HOST_SURFACES =
            new WeakHashMap<>();
    private static final ArrayList<ActivityRecord> ACTIVITY_STACK = new ArrayList<>();
    private static volatile Activity currentActivity;

    /** Publishes a host-window resize through WindowManager/ViewRootImpl. */
    static void resizeDisplay(int width, int height) {
        if (width <= 0 || height <= 0
                || (DISPLAY_WIDTH == width && DISPLAY_HEIGHT == height)) {
            return;
        }
        DISPLAY_WIDTH = width;
        DISPLAY_HEIGHT = height;
        Activity activity = currentActivity;
        if (activity == null) return;
        View decor = activity.getWindow().getDecorView();
        // requestLayout crosses the ViewRootImpl traversal barrier. Its next
        // WMS relayout reads the new display bounds below and lets
        // ThreadedRenderer record/submit exactly one resized frame.
        decor.requestLayout();
        decor.invalidate();
    }

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

    private static final class HostSurfaceState {
        final int[] geometry = new int[4];
        final WeakHashMap<SurfaceHolder.Callback, Boolean> announced =
                new WeakHashMap<>();

        HostSurfaceState(int x, int y, int width, int height) {
            setGeometry(x, y, width, height);
        }

        void setGeometry(int x, int y, int width, int height) {
            geometry[0] = x;
            geometry[1] = y;
            geometry[2] = width;
            geometry[3] = height;
        }
    }

    private DarwinServiceBridge() {}

    /** Installs the Activity already launched by the native process bootstrap. */
    static void installInitialActivity(Activity activity) {
        if (System.getenv("DARWIN_ART_DEBUG_SECURITY") != null) {
            try {
                String algorithm = javax.net.ssl.TrustManagerFactory.getDefaultAlgorithm();
                javax.net.ssl.TrustManagerFactory factory =
                        javax.net.ssl.TrustManagerFactory.getInstance(algorithm);
                factory.init((java.security.KeyStore) null);
                StringBuilder managers = new StringBuilder();
                for (javax.net.ssl.TrustManager manager : factory.getTrustManagers()) {
                    if (managers.length() != 0) managers.append(',');
                    managers.append(manager.getClass().getName());
                }
                Log.i("DarwinServiceBridge", "security default=" + algorithm
                        + " provider=" + factory.getProvider().getName()
                        + " managers=" + managers);
            } catch (Throwable error) {
                Log.e("DarwinServiceBridge", "security provider diagnostic failed", error);
            }
        }
        currentActivity = activity;
        ACTIVITY_STACK.clear();
        ACTIVITY_STACK.add(new ActivityRecord(activity, activityToken(activity), -1));
    }

    /** Completes ActivityThread's server/client visibility handshake after addView(). */
    static void completeActivityWindowVisibility(Activity activity, View decor) {
        try {
            Field activityDecor = Activity.class.getDeclaredField("mDecor");
            Field windowAdded = Activity.class.getDeclaredField("mWindowAdded");
            Field visibleFromServer =
                    Activity.class.getDeclaredField("mVisibleFromServer");
            Field visibleFromClient =
                    Activity.class.getDeclaredField("mVisibleFromClient");
            Method makeVisible = Activity.class.getDeclaredMethod("makeVisible");
            activityDecor.setAccessible(true);
            windowAdded.setAccessible(true);
            visibleFromServer.setAccessible(true);
            visibleFromClient.setAccessible(true);
            makeVisible.setAccessible(true);
            // ActivityThread.handleResumeActivity() stores Activity.mDecor
            // before adding it to WindowManager, then completes the server
            // visibility handshake. Our direct addView path must preserve the
            // same Activity-owned state or makeVisible() dereferences null.
            activityDecor.set(activity, decor);
            windowAdded.setBoolean(activity, true);
            visibleFromServer.setBoolean(activity, true);
            if (visibleFromClient.getBoolean(activity)) {
                makeVisible.invoke(activity);
            }
            scheduleViewTreeDump(activity, decor);
        } catch (ReflectiveOperationException error) {
            throw new IllegalStateException(
                    "could not complete Activity window visibility", error);
        }
    }

    /** ContextImpl-style entry into the process ActivityTaskManager service. */
    public static void startActivityFromContext(Intent intent, Bundle options) {
        if (intent == null) throw new NullPointerException("intent");
        Intent launchIntent = new Intent(intent);
        Log.i("DarwinServiceBridge", "context startActivity action="
                + launchIntent.getAction() + " component="
                + launchIntent.getComponent());
        MAIN_HANDLER.post(() -> launchLocalActivity(launchIntent, -1));
    }

    /** Mirrors ActivityThread's process teardown before the host destroys ART. */
    public static void shutdownActivities() {
        try {
            Class<?> activityClass = Activity.class;
            Method performPause = activityClass.getDeclaredMethod("performPause");
            Method performStop = activityClass.getDeclaredMethod(
                    "performStop", boolean.class, String.class);
            Method performDestroy = activityClass.getDeclaredMethod("performDestroy");
            performPause.setAccessible(true);
            performStop.setAccessible(true);
            performDestroy.setAccessible(true);
            Object global = Class.forName("android.view.WindowManagerGlobal")
                    .getMethod("getInstance").invoke(null);
            Method removeView = global.getClass().getDeclaredMethod(
                    "removeView", View.class, boolean.class);
            removeView.setAccessible(true);
            for (int index = ACTIVITY_STACK.size() - 1; index >= 0; --index) {
                Activity activity = ACTIVITY_STACK.get(index).activity;
                if (activity == null) continue;
                if (activity == currentActivity) performPause.invoke(activity);
                View decor = activity.getWindow().getDecorView();
                deactivateHostSurfaces(decor);
                try {
                    removeView.invoke(global, decor, Boolean.TRUE);
                } catch (Throwable ignored) {
                    // A finishing Activity may already have removed its decor.
                }
                performStop.invoke(activity, Boolean.FALSE,
                        "darwin-art process shutdown");
                performDestroy.invoke(activity);
            }
            ACTIVITY_STACK.clear();
            currentActivity = null;
            Log.i("DarwinServiceBridge", "destroyed Activities for process shutdown");
        } catch (Throwable error) {
            Log.e("DarwinServiceBridge", "Activity process shutdown failed", error);
        }
    }

    private static void scheduleViewTreeDump(Activity owner, View root) {
        if (!"1".equals(System.getenv("DARWIN_ART_DEBUG_VIEW_TREE"))) return;
        scheduleViewTreeDump(owner, root, 8);
    }

    private static void scheduleViewTreeDump(Activity owner, View root, int remaining) {
        if (remaining <= 0) return;
        MAIN_HANDLER.postDelayed(() -> {
            if (currentActivity != owner) return;
            StringBuilder tree = new StringBuilder("Android view tree:\n");
            appendViewTree(root, tree, 0, new int[] {0});
            Log.i("DarwinServiceBridge", tree.toString());
            scheduleViewTreeDump(owner, root, remaining - 1);
        }, 750L);
    }

    private static void appendViewTree(
            View view, StringBuilder tree, int depth, int[] count) {
        if (view == null || count[0]++ >= 256) return;
        for (int index = 0; index < depth; ++index) tree.append("  ");
        tree.append(view.getClass().getName())
                .append(" id=0x").append(Integer.toHexString(view.getId()))
                .append(" visibility=").append(view.getVisibility())
                .append(" bounds=").append(view.getLeft()).append(',')
                .append(view.getTop()).append('-').append(view.getRight())
                .append(',').append(view.getBottom())
                .append(" measured=").append(view.getMeasuredWidth()).append('x')
                .append(view.getMeasuredHeight())
                .append(" alpha=").append(view.getAlpha())
                .append(" attached=").append(view.isAttachedToWindow())
                .append(" laidOut=").append(view.isLaidOut());
        if (view.isClickable()) {
            try {
                Field listenerInfoField = View.class.getDeclaredField("mListenerInfo");
                listenerInfoField.setAccessible(true);
                Object listenerInfo = listenerInfoField.get(view);
                Field clickField = listenerInfo == null ? null
                        : listenerInfo.getClass().getDeclaredField("mOnClickListener");
                if (clickField != null) clickField.setAccessible(true);
                Object listener = clickField == null ? null : clickField.get(listenerInfo);
                tree.append(" clickListener=").append(
                        listener == null ? "null" : listener.getClass().getName());
            } catch (ReflectiveOperationException ignored) {
                tree.append(" clickListener=<unavailable>");
            }
        }
        if (view instanceof android.widget.TextView) {
            tree.append(" text=")
                    .append(((android.widget.TextView) view).getText());
        }
        tree.append('\n');
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            for (int index = 0; index < group.getChildCount(); ++index) {
                appendViewTree(group.getChildAt(index), tree, depth + 1, count);
            }
        }
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
            int width = surfaceView.getWidth();
            int height = surfaceView.getHeight();
            // SurfaceFlinger does not publish a producer until layout has
            // produced a real frame. In particular, never turn a transient
            // 0x0 SurfaceView into a permanently "activated" host surface.
            if (width > 0 && height > 0 && surfaceView.isAttachedToWindow()) {
                int[] location = new int[2];
                surfaceView.getLocationInWindow(location);
                HostSurfaceState state = HOST_SURFACES.get(surfaceView);
                boolean geometryChanged = state == null
                        || state.geometry[0] != location[0]
                        || state.geometry[1] != location[1]
                        || state.geometry[2] != width
                        || state.geometry[3] != height;
                SurfaceHolder holder = surfaceView.getHolder();
                if (state == null) {
                    state = new HostSurfaceState(
                            location[0], location[1], width, height);
                    HOST_SURFACES.put(surfaceView, state);
                }
                nativeConfigureHostSurface(surfaceView, holder.getSurface(),
                        location[0], location[1], width, height);
                if (hasFrameworkViewRoot(surfaceView)) {
                    // ViewRootImpl/SurfaceView owns callback ordering once the
                    // real WMS traversal path is active. The host compositor
                    // only binds its producer and geometry; dispatching the
                    // callbacks again violates SurfaceHolder's exactly-once
                    // lifecycle (Chromium asserts on duplicate creation).
                    if (geometryChanged) {
                        state.setGeometry(location[0], location[1], width, height);
                    }
                    return;
                }
                ArrayList<SurfaceHolder.Callback> callbacks =
                        hostSurfaceCallbacks(surfaceView);
                boolean hasNewCallback = false;
                takeOverHostSurfaceLifecycle(surfaceView);
                // Detached legacy probes have no ViewRoot traversal to publish
                // callbacks, so retain their bounded compatibility path.
                for (SurfaceHolder.Callback callback : callbacks) {
                    if (!state.announced.containsKey(callback)) {
                        callback.surfaceCreated(holder);
                        state.announced.put(callback, Boolean.TRUE);
                        hasNewCallback = true;
                    }
                }
                if (geometryChanged || hasNewCallback) {
                    for (SurfaceHolder.Callback callback : callbacks) {
                        callback.surfaceChanged(holder, 1, width, height);
                    }
                    state.setGeometry(location[0], location[1], width, height);
                    Log.i("DarwinServiceBridge", "host SurfaceView "
                            + (hasNewCallback ? "created " : "changed ")
                            + surfaceView.getClass().getName() + " callbacks="
                            + callbacks.size() + " geometry=" + location[0] + ","
                            + location[1] + " " + width + "x" + height);
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

    private static boolean hasFrameworkViewRoot(View view) {
        try {
            Method getViewRoot = View.class.getDeclaredMethod("getViewRootImpl");
            getViewRoot.setAccessible(true);
            return getViewRoot.invoke(view) != null;
        } catch (ReflectiveOperationException error) {
            throw new IllegalStateException("could not inspect ViewRoot ownership", error);
        }
    }

    private static boolean isInViewTree(View view, View root) {
        View current = view;
        while (current != null) {
            if (current == root) return true;
            ViewParent parent = current.getParent();
            current = parent instanceof View ? (View) parent : null;
        }
        return false;
    }

    private static void destroyHostSurface(SurfaceView surfaceView) {
        HostSurfaceState state = HOST_SURFACES.remove(surfaceView);
        if (state == null) return;
        SurfaceHolder holder = surfaceView.getHolder();
        Surface surface = holder.getSurface();
        try {
            // SurfaceView replaces its BufferQueue producer before notifying
            // callbacks. Match that ordering so clients observe !isValid()
            // from surfaceDestroyed() and receive a fresh native identity if
            // the same Java SurfaceView is attached again.
            Field nativeObject = Surface.class.getDeclaredField("mNativeObject");
            nativeObject.setAccessible(true);
            nativeObject.setLong(surface, 0L);
        } catch (ReflectiveOperationException error) {
            throw new IllegalStateException(
                    "could not invalidate detached host Surface", error);
        }
        for (SurfaceHolder.Callback callback :
                new ArrayList<>(state.announced.keySet())) {
            callback.surfaceDestroyed(holder);
        }
        Log.i("DarwinServiceBridge", "host SurfaceView destroyed "
                + surfaceView.getClass().getName());
    }

    private static void syncHostSurfaces(View root) {
        // SurfaceView removal is independent from Activity destruction. Chrome
        // swaps opaque/translucent compositor SurfaceViews by removeView() and
        // waits for surfaceDestroyed() before reattaching the selected one.
        // Reconcile the retained producer table against the real View tree on
        // every display pulse, just as SurfaceFlinger/WindowManager would.
        for (SurfaceView surfaceView :
                new ArrayList<>(HOST_SURFACES.keySet())) {
            if (surfaceView == null || !surfaceView.isAttachedToWindow()
                    || !isInViewTree(surfaceView, root)) {
                if (surfaceView != null) destroyHostSurface(surfaceView);
            }
        }
        activateHostSurfaces(root);
    }

    private static void takeOverHostSurfaceLifecycle(SurfaceView surfaceView) {
        try {
            Field haveFrame = SurfaceView.class.getDeclaredField("mHaveFrame");
            Field drawFinished = SurfaceView.class.getDeclaredField("mDrawFinished");
            Field drawListener = SurfaceView.class.getDeclaredField("mDrawListener");
            Field scrollListener =
                    SurfaceView.class.getDeclaredField("mScrollChangedListener");
            haveFrame.setAccessible(true);
            drawFinished.setAccessible(true);
            drawListener.setAccessible(true);
            scrollListener.setAccessible(true);
            android.view.ViewTreeObserver observer = surfaceView.getViewTreeObserver();
            observer.removeOnPreDrawListener(
                    (android.view.ViewTreeObserver.OnPreDrawListener)
                            drawListener.get(surfaceView));
            observer.removeOnScrollChangedListener(
                    (android.view.ViewTreeObserver.OnScrollChangedListener)
                            scrollListener.get(surfaceView));
            // Every framework updateSurface() path returns before touching
            // SurfaceControl while the detached compositor owns the producer.
            haveFrame.setBoolean(surfaceView, false);
            // updateSurface() normally reaches performDrawFinished() after
            // SurfaceFlinger commits the child layer. The Darwin compositor
            // has already published that layer before callbacks are delivered,
            // so complete the same framework state transition here. Without
            // it SurfaceView never records Canvas.punchHole(), causing the
            // parent HWUI background to cover the child surface (or forcing a
            // child-last workaround that incorrectly covers overlay controls).
            if (!drawFinished.getBoolean(surfaceView)) {
                drawFinished.setBoolean(surfaceView, true);
                if (surfaceView.getParent() != null) {
                    surfaceView.getParent().requestTransparentRegion(surfaceView);
                }
                surfaceView.invalidate();
            }
        } catch (ReflectiveOperationException error) {
            throw new IllegalStateException(
                    "could not acquire detached SurfaceView lifecycle", error);
        }
    }

    /** Called by the host compositor after each Android main-message drain. */
    static void activateCurrentHostSurfaces() {
        Activity activity = currentActivity;
        if (activity != null) {
            View decor = activity.getWindow().getDecorView();
            // SurfaceFlinger observes the hierarchy produced by ViewRoot; it
            // never performs measure/layout itself. Re-running the detached
            // zero-size repair from every compositor pulse forced legitimate
            // 0x0 Chrome placeholders to request layout forever, which in
            // turn caused a full HWUI re-record on every vsync. ViewRootImpl is
            // now the only owner of measure/layout for attached product views.
            syncHostSurfaces(decor);
        }
    }

    private static void deactivateHostSurfaces(View view) {
        if (view instanceof SurfaceView) {
            SurfaceView surfaceView = (SurfaceView) view;
            destroyHostSurface(surfaceView);
        }
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            for (int index = 0; index < group.getChildCount(); ++index) {
                deactivateHostSurfaces(group.getChildAt(index));
            }
        }
    }

    @SuppressWarnings("unchecked")
    private static ArrayList<SurfaceHolder.Callback> hostSurfaceCallbacks(
            SurfaceView surfaceView) {
        try {
            Field callbacks = SurfaceView.class.getDeclaredField("mCallbacks");
            callbacks.setAccessible(true);
            ArrayList<SurfaceHolder.Callback> registered =
                    (ArrayList<SurfaceHolder.Callback>) callbacks.get(surfaceView);
            // Match SurfaceView.getSurfaceCallbacks(): take a stable snapshot
            // because callbacks may add/remove registrations while notified.
            return new ArrayList<>(registered);
        } catch (Throwable error) {
            Log.e("DarwinServiceBridge", "could not enumerate SurfaceHolder callbacks", error);
            return new ArrayList<>();
        }
    }

    /** Rebinds the Metal/HWUI owner after an Android-owned Activity launch. */
    private static native boolean nativeInstallActivity(
            Activity activity, View decorView);
    private static native String nativeChooseDocument(String mimeType);
    private static native String[] nativeChooseSaveDocument(
            String mimeType, String suggestedName);
    private static native void nativeConfigureHostSurface(
            SurfaceView surfaceView, android.view.Surface surface,
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

            Binder userBinder = new Binder();
            Class<?> userInterface = Class.forName("android.os.IUserManager");
            Object userService = Proxy.newProxyInstance(
                    userInterface.getClassLoader(), new Class<?>[] {userInterface},
                    new UserManagerHandler());
            attach(userBinder, userService, "android.os.IUserManager");

            Binder devicePolicyBinder = new DevicePolicyServiceBinder();

            Binder usageStatsBinder = new Binder();
            Class<?> usageStatsInterface = Class.forName(
                    "android.app.usage.IUsageStatsManager");
            Object usageStatsService = Proxy.newProxyInstance(
                    usageStatsInterface.getClassLoader(),
                    new Class<?>[] {usageStatsInterface},
                    (proxy, method, args) -> {
                        if ("asBinder".equals(method.getName())) return usageStatsBinder;
                        if ("getAppStandbyBucket".equals(method.getName())) {
                            return Integer.valueOf(
                                    android.app.usage.UsageStatsManager.STANDBY_BUCKET_ACTIVE);
                        }
                        if ("isAppStandbyEnabled".equals(method.getName())) {
                            return Boolean.TRUE;
                        }
                        if ("isAppInactive".equals(method.getName())) {
                            return Boolean.FALSE;
                        }
                        return defaultValue(method.getReturnType());
                    });
            attach(usageStatsBinder, usageStatsService,
                    "android.app.usage.IUsageStatsManager");

            Binder roleBinder = new Binder();
            Class<?> roleInterface = Class.forName("android.app.role.IRoleManager");
            Object roleService = Proxy.newProxyInstance(
                    roleInterface.getClassLoader(), new Class<?>[] {roleInterface},
                    (proxy, method, args) -> {
                        String name = method.getName();
                        if ("asBinder".equals(name)) return roleBinder;
                        if ("isRoleAvailableAsUser".equals(name)
                                || "isRoleVisibleAsUser".equals(name)
                                || "isApplicationVisibleForRoleAsUser".equals(name)) {
                            return Boolean.TRUE;
                        }
                        if ("isRoleHeldAsUser".equals(name)
                                || "isBypassingRoleQualification".equals(name)) {
                            return Boolean.FALSE;
                        }
                        if ("getRoleHoldersAsUser".equals(name)
                                || "getHeldRolesFromControllerAsUser".equals(name)
                                || "getDefaultHoldersForTestAsUser".equals(name)) {
                            return new ArrayList<>();
                        }
                        if ("setBrowserRoleHolder".equals(name)
                                || "addRoleHolderFromControllerAsUser".equals(name)
                                || "removeRoleHolderFromControllerAsUser".equals(name)) {
                            return Boolean.TRUE;
                        }
                        return defaultValue(method.getReturnType());
                    });
            attach(roleBinder, roleService, "android.app.role.IRoleManager");

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
                    (proxy, method, args) -> {
                        String name = method.getName();
                        if ("asBinder".equals(name)) return inputMethodBinder;
                        if (System.getenv("DARWIN_ART_DEBUG_INPUT_LATENCY") != null
                                && (name.contains("Input") || name.contains("Client"))) {
                            Log.i("DarwinInputMethod", "system service call " + name);
                        }
                        if ("getInputMethodList".equals(name)
                                || "getEnabledInputMethodList".equals(name)) {
                            return emptyFrameworkContainer(method.getReturnType(), "empty");
                        }
                        if ("startInputOrWindowGainedFocus".equals(name)
                                || "startInputOrWindowGainedFocusAsync".equals(name)) {
                            // A connected physical keyboard does not require a
                            // soft IME session, but Android system_server still
                            // returns a non-null result. InputMethodManager uses
                            // that result to retain the served View and its
                            // renderer InputConnection. Returning null leaves
                            // Chromium's ImeAdapter detached, so hardware key
                            // events are consumed without reaching Blink.
                            Class<?> resultClass = Class.forName(
                                    "com.android.internal.inputmethod.InputBindResult");
                            Object result = resultClass.getField("NO_IME").get(null);
                            if ("startInputOrWindowGainedFocusAsync".equals(name)) {
                                Object client = args[1];
                                int sequence = -1;
                                for (int index = args.length - 1; index >= 0; index--) {
                                    if (args[index] instanceof Integer) {
                                        sequence = (Integer) args[index];
                                        break;
                                    }
                                }
                                client.getClass()
                                        .getMethod("onStartInputResult", resultClass, int.class)
                                        .invoke(client, result, sequence);
                                return null;
                            }
                            return result;
                        }
                        return defaultValue(method.getReturnType());
                    });
            attach(inputMethodBinder, inputMethodService,
                    "com.android.internal.view.IInputMethodManager");

            // Editable Chromium content obtains TextServicesManager before it
            // creates the renderer-side InputConnection. Android always
            // publishes this system_server service even when spell checking
            // is disabled. Expose that same disabled-but-present contract:
            // isSpellCheckerEnabled() is false and every optional object is
            // null, so TextServicesManager.newSpellCheckerSession() returns
            // null normally instead of the application dereferencing a null
            // manager returned by Context.getSystemService().
            Binder textServicesBinder = new Binder();
            Class<?> textServicesInterface = Class.forName(
                    "com.android.internal.textservice.ITextServicesManager");
            Object textServicesService = Proxy.newProxyInstance(
                    textServicesInterface.getClassLoader(),
                    new Class<?>[] {textServicesInterface},
                    (proxy, method, args) -> defaultValue(method.getReturnType()));
            attach(textServicesBinder, textServicesService,
                    "com.android.internal.textservice.ITextServicesManager");

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

            // UiModeManager is a framework facade over IUiModeManager.  Keep
            // Chromium on Android's normal accessibility/theme path and
            // describe the Darwin desktop as a normal, light-mode display
            // with the platform's default contrast.
            Binder uiModeBinder = new Binder();
            Class<?> uiModeInterface = Class.forName("android.app.IUiModeManager");
            Object uiModeService = Proxy.newProxyInstance(
                    uiModeInterface.getClassLoader(),
                    new Class<?>[] {uiModeInterface},
                    (proxy, method, args) -> {
                        String name = method.getName();
                        if ("asBinder".equals(name)) return uiModeBinder;
                        if ("getCurrentModeType".equals(name)) {
                            return Integer.valueOf(Configuration.UI_MODE_TYPE_NORMAL);
                        }
                        if ("getNightMode".equals(name)) {
                            return Integer.valueOf(android.app.UiModeManager.MODE_NIGHT_NO);
                        }
                        if ("getProjectingPackages".equals(name)) {
                            return new ArrayList<String>();
                        }
                        if ("getContrast".equals(name)) return Float.valueOf(0.0f);
                        return defaultValue(method.getReturnType());
                    });
            attach(uiModeBinder, uiModeService, "android.app.IUiModeManager");

            // Android's modular Bluetooth framework obtains its manager
            // binder through BluetoothFrameworkInitializer rather than the
            // ordinary SystemServiceRegistry. Publish an adapter in the OFF
            // state: callers receive a real BluetoothAdapter object while no
            // host radio capability is fabricated.
            Binder bluetoothManagerBinder = new Binder();
            Class<?> bluetoothManagerInterface =
                    Class.forName("android.bluetooth.IBluetoothManager");
            Object bluetoothManagerService = Proxy.newProxyInstance(
                    bluetoothManagerInterface.getClassLoader(),
                    new Class<?>[] {bluetoothManagerInterface},
                    (proxy, method, args) -> {
                        String name = method.getName();
                        if ("asBinder".equals(name)) return bluetoothManagerBinder;
                        if ("getState".equals(name)) return Integer.valueOf(10);
                        return defaultValue(method.getReturnType());
                    });
            attach(bluetoothManagerBinder, bluetoothManagerService,
                    "android.bluetooth.IBluetoothManager");
            Class<?> serviceManagerType = Class.forName(
                    "android.os.BluetoothServiceManager");
            Object bluetoothServiceManager =
                    serviceManagerType.getDeclaredConstructor().newInstance();
            Class<?> initializer = Class.forName(
                    "android.bluetooth.BluetoothFrameworkInitializer");
            Method getBluetoothServiceManager = initializer.getMethod(
                    "getBluetoothServiceManager");
            if (getBluetoothServiceManager.invoke(null) == null) {
                initializer.getMethod("setBluetoothServiceManager", serviceManagerType)
                        .invoke(null, bluetoothServiceManager);
            }

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
                    (proxy, method, args) -> {
                        if ("asBinder".equals(method.getName())) {
                            return notificationBinder;
                        }
                        if ("android.content.pm.ParceledListSlice".equals(
                                method.getReturnType().getName())) {
                            return emptyFrameworkContainer(
                                    method.getReturnType(), "emptyList");
                        }
                        return defaultValue(method.getReturnType());
                    });
            attach(notificationBinder, notificationService,
                    "android.app.INotificationManager");

            // PlayerBase registers every AudioTrack with system_server's
            // IAudioService before AudioTrack.native_setup is usable.  Keep
            // that Android lifecycle intact and allocate stable process-local
            // player ids for media apps running without system_server.
            Binder audioBinder = new AudioServiceBinder();

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

            Binder uriGrantsBinder = new Binder();
            Class<?> uriGrantsInterface = Class.forName(
                    "android.app.IUriGrantsManager");
            Object uriGrantsService = Proxy.newProxyInstance(
                    uriGrantsInterface.getClassLoader(),
                    new Class<?>[] {uriGrantsInterface},
                    Proxy.getInvocationHandler(activityManagerService));
            attach(uriGrantsBinder, uriGrantsService,
                    "android.app.IUriGrantsManager");

            // Keep the framework AccountManager/IAccountManager Binder
            // contract intact and publish an empty physical-user account set.
            // Apps must observe a valid empty result rather than a missing
            // system service whose asynchronous request never completes.
            Binder accountBinder = new Binder();
            Class<?> accountInterface = Class.forName(
                    "android.accounts.IAccountManager");
            Object accountService = Proxy.newProxyInstance(
                    accountInterface.getClassLoader(),
                    new Class<?>[] {accountInterface},
                    (proxy, method, args) -> {
                        String name = method.getName();
                        Log.i("DarwinServiceBridge", "IAccountManager " + name);
                        if ("asBinder".equals(name)) return accountBinder;
                        if ("getAuthenticatorTypes".equals(name)) {
                            return new android.accounts.AuthenticatorDescription[0];
                        }
                        if ("getAccountsForPackage".equals(name)
                                || "getAccountsByTypeForPackage".equals(name)
                                || "getAccountsAsUser".equals(name)) {
                            return new android.accounts.Account[0];
                        }
                        if ("getPackagesAndVisibilityForAccount".equals(name)
                                || "getAccountsAndVisibilityForPackage".equals(name)) {
                            return new java.util.HashMap<>();
                        }
                        if (args != null && args.length > 0 && args[0] != null) {
                            Bundle result = new Bundle();
                            if ("getAccountsByFeatures".equals(name)) {
                                result.putParcelableArray(
                                        android.accounts.AccountManager.KEY_ACCOUNTS,
                                        new android.accounts.Account[0]);
                            } else if ("hasFeatures".equals(name)) {
                                result.putBoolean(
                                        android.accounts.AccountManager.KEY_BOOLEAN_RESULT,
                                        false);
                            }
                            try {
                                Method onResult = args[0].getClass().getMethod(
                                        "onResult", Bundle.class);
                                onResult.setAccessible(true);
                                onResult.invoke(args[0], result);
                                return null;
                            } catch (NoSuchMethodException ignored) {
                                // Synchronous IAccountManager calls continue
                                // through the typed defaults below.
                            }
                        }
                        return defaultValue(method.getReturnType());
                    });
            attach(accountBinder, accountService, "android.accounts.IAccountManager");

            // Environment.UserEnvironment obtains shared-storage roots from
            // the system_server mount service. Keep that Android contract and
            // expose one primary emulated volume backed only by this app's
            // runtime-owned data directory; never expose the host filesystem
            // root as Android shared storage.
            Object primaryVolume = createPrimaryStorageVolume();
            Binder storageBinder = new Binder();
            Class<?> storageInterface = Class.forName(
                    "android.os.storage.IStorageManager");
            Object storageService = Proxy.newProxyInstance(
                    storageInterface.getClassLoader(),
                    new Class<?>[] {storageInterface},
                    (proxy, method, args) -> {
                        if ("getVolumeList".equals(method.getName())) {
                            Object array = java.lang.reflect.Array.newInstance(
                                    primaryVolume.getClass(), 1);
                            java.lang.reflect.Array.set(array, 0, primaryVolume);
                            return array;
                        }
                        if ("getVolumeState".equals(method.getName())) {
                            return android.os.Environment.MEDIA_MOUNTED;
                        }
                        return defaultValue(method.getReturnType());
                    });
            attach(storageBinder, storageService,
                    "android.os.storage.IStorageManager");

            // MediaSession is a framework facade over the system_server
            // ISessionManager service. Keep that Android boundary intact for
            // media apps: MediaSessionManager creates a process-local ISession,
            // whose token exposes the paired ISessionController interface.
            // Playback remains app-owned (for example libvlc); this service
            // only supplies Android's session/control coordination contract.
            MediaSessionInterfaceHandler mediaSessionManagerHandler =
                    new MediaSessionInterfaceHandler(
                            "android.media.session.ISessionManager", null);
            Binder mediaSessionBinder = mediaSessionManagerHandler.binder;
            // The legacy framework MediaRouter still obtains its coordination
            // endpoint directly from ServiceManager.  Publish a typed service
            // that represents this host's single local audio/video route.
            // MediaRouter itself creates and owns that default route; null
            // state here means there are no additional remote routes.
            Binder mediaRouterBinder = new Binder();
            Class<?> mediaRouterInterface = Class.forName(
                    "android.media.IMediaRouterService");
            Object mediaRouterService = Proxy.newProxyInstance(
                    mediaRouterInterface.getClassLoader(),
                    new Class<?>[] {mediaRouterInterface},
                    (proxy, method, args) -> {
                        if ("asBinder".equals(method.getName())) {
                            return mediaRouterBinder;
                        }
                        return defaultValue(method.getReturnType());
                    });
            attach(mediaRouterBinder, mediaRouterService,
                    "android.media.IMediaRouterService");
            MediaSessionInterfaceHandler trustManagerHandler =
                    new MediaSessionInterfaceHandler(
                            "android.app.trust.ITrustManager", null);
            Binder trustBinder = trustManagerHandler.binder;

            Class<?> metadataClass = Class.forName("android.os.ServiceWithMetadata");
            Class<?> serviceClass = Class.forName("android.os.Service");
            Object displayServiceValue = serviceValue(
                    metadataClass, serviceClass, displayBinder);
            Object activityServiceValue = serviceValue(
                    metadataClass, serviceClass, activityBinder);
            Object activityManagerServiceValue = serviceValue(
                    metadataClass, serviceClass, activityManagerBinder);
            Object userServiceValue = serviceValue(
                    metadataClass, serviceClass, userBinder);
            Object devicePolicyServiceValue = serviceValue(
                    metadataClass, serviceClass, devicePolicyBinder);
            Object usageStatsServiceValue = serviceValue(
                    metadataClass, serviceClass, usageStatsBinder);
            Object roleServiceValue = serviceValue(
                    metadataClass, serviceClass, roleBinder);
            Object inputMethodServiceValue = serviceValue(
                    metadataClass, serviceClass, inputMethodBinder);
            Object textServicesServiceValue = serviceValue(
                    metadataClass, serviceClass, textServicesBinder);
            Object inputServiceValue = serviceValue(metadataClass, serviceClass, inputBinder);
            Object uiModeServiceValue = serviceValue(
                    metadataClass, serviceClass, uiModeBinder);
            Object bluetoothManagerServiceValue = serviceValue(
                    metadataClass, serviceClass, bluetoothManagerBinder);
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
            Object audioServiceValue = serviceValue(
                    metadataClass, serviceClass, audioBinder);
            Object voiceInteractionServiceValue = serviceValue(
                    metadataClass, serviceClass, voiceInteractionBinder);
            Object searchServiceValue = serviceValue(
                    metadataClass, serviceClass, searchBinder);
            Object clipboardServiceValue = serviceValue(
                    metadataClass, serviceClass, clipboardBinder);
            Object uriGrantsServiceValue = serviceValue(
                    metadataClass, serviceClass, uriGrantsBinder);
            Object accountServiceValue = serviceValue(
                    metadataClass, serviceClass, accountBinder);
            Object storageServiceValue = serviceValue(
                    metadataClass, serviceClass, storageBinder);
            Object mediaSessionServiceValue = serviceValue(
                    metadataClass, serviceClass, mediaSessionBinder);
            Object mediaRouterServiceValue = serviceValue(
                    metadataClass, serviceClass, mediaRouterBinder);
            Object trustServiceValue = serviceValue(
                    metadataClass, serviceClass, trustBinder);
            Object missingServiceValue = serviceValue(
                    metadataClass, serviceClass, null);

            Class<?> managerInterface = Class.forName("android.os.IServiceManager");
            Object manager = Proxy.newProxyInstance(
                    managerInterface.getClassLoader(),
                    new Class<?>[] {managerInterface},
                    new ManagerHandler(displayServiceValue, activityManagerServiceValue,
                            activityServiceValue, userServiceValue,
                            devicePolicyServiceValue,
                            usageStatsServiceValue, roleServiceValue,
                            inputMethodServiceValue, textServicesServiceValue,
                            inputServiceValue, uiModeServiceValue,
                            bluetoothManagerServiceValue,
                            windowServiceValue,
                            accessibilityServiceValue, sensitiveContentServiceValue,
                            contentServiceValue, notificationServiceValue, audioServiceValue,
                            voiceInteractionServiceValue, searchServiceValue,
                            clipboardServiceValue, uriGrantsServiceValue,
                            accountServiceValue, storageServiceValue,
                            mediaSessionServiceValue, mediaRouterServiceValue,
                            trustServiceValue,
                            missingServiceValue));
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

    private static Object createPrimaryStorageVolume() throws Exception {
        String externalDirectory =
                System.getenv("DARWIN_ART_APK_APP_EXTERNAL_DIR");
        if (externalDirectory == null || externalDirectory.isEmpty()) {
            throw new IllegalStateException(
                    "app external-storage directory is unavailable");
        }
        // Java File operations are Android guest operations once libcore is
        // attached to the filesystem facade.  Publishing the Darwin backing
        // directory here would both fail capability resolution and leak a
        // host path into Environment.  The launcher creates this exact guest
        // directory before sealing the immutable storage mount.
        File root = new File(externalDirectory);
        if (!root.isDirectory() && !root.mkdirs()) {
            throw new IllegalStateException(
                    "could not create app-scoped external storage");
        }
        Class<?> type = Class.forName("android.os.storage.StorageVolume");
        Constructor<?> constructor = type.getDeclaredConstructor(
                String.class, File.class, File.class, String.class,
                boolean.class, boolean.class, boolean.class, boolean.class,
                boolean.class, long.class, android.os.UserHandle.class,
                java.util.UUID.class, String.class, String.class);
        constructor.setAccessible(true);
        return constructor.newInstance(
                "emulated;0", root, root, "Darwin ART shared storage",
                Boolean.TRUE, Boolean.FALSE, Boolean.TRUE, Boolean.FALSE,
                Boolean.FALSE, Long.valueOf(0L), android.os.Process.myUserHandle(),
                null, null, android.os.Environment.MEDIA_MOUNTED);
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
        private final Object userService;
        private final Object devicePolicyService;
        private final Object usageStatsService;
        private final Object roleService;
        private final Object inputMethodService;
        private final Object textServicesService;
        private final Object inputService;
        private final Object uiModeService;
        private final Object bluetoothManagerService;
        private final Object windowService;
        private final Object accessibilityService;
        private final Object sensitiveContentService;
        private final Object contentService;
        private final Object notificationService;
        private final Object audioService;
        private final Object voiceInteractionService;
        private final Object searchService;
        private final Object clipboardService;
        private final Object uriGrantsService;
        private final Object accountService;
        private final Object storageService;
        private final Object mediaSessionService;
        private final Object mediaRouterService;
        private final Object trustService;
        private final Object missingService;

        ManagerHandler(Object displayService, Object activityManagerService,
                Object activityService, Object userService,
                Object devicePolicyService,
                Object usageStatsService, Object roleService,
                Object inputMethodService, Object textServicesService,
                Object inputService, Object uiModeService,
                Object bluetoothManagerService,
                Object windowService,
                Object accessibilityService, Object sensitiveContentService,
                Object contentService, Object notificationService,
                Object audioService,
                Object voiceInteractionService, Object searchService,
                Object clipboardService, Object uriGrantsService,
                Object accountService, Object storageService,
                Object mediaSessionService, Object mediaRouterService,
                Object trustService,
                Object missingService) {
            this.displayService = displayService;
            this.activityManagerService = activityManagerService;
            this.activityService = activityService;
            this.userService = userService;
            this.devicePolicyService = devicePolicyService;
            this.usageStatsService = usageStatsService;
            this.roleService = roleService;
            this.inputMethodService = inputMethodService;
            this.textServicesService = textServicesService;
            this.inputService = inputService;
            this.uiModeService = uiModeService;
            this.bluetoothManagerService = bluetoothManagerService;
            this.windowService = windowService;
            this.accessibilityService = accessibilityService;
            this.sensitiveContentService = sensitiveContentService;
            this.contentService = contentService;
            this.notificationService = notificationService;
            this.audioService = audioService;
            this.voiceInteractionService = voiceInteractionService;
            this.searchService = searchService;
            this.clipboardService = clipboardService;
            this.uriGrantsService = uriGrantsService;
            this.accountService = accountService;
            this.storageService = storageService;
            this.mediaSessionService = mediaSessionService;
            this.mediaRouterService = mediaRouterService;
            this.trustService = trustService;
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
                if ("user".equals(args[0])) return userService;
                if ("device_policy".equals(args[0])) return devicePolicyService;
                if ("usagestats".equals(args[0])) return usageStatsService;
                if ("role".equals(args[0])) return roleService;
                if ("activity_task".equals(args[0])) {
                    return activityService;
                }
                if ("input_method".equals(args[0])) return inputMethodService;
                if ("textservices".equals(args[0])) return textServicesService;
                if ("input".equals(args[0])) return inputService;
                if ("uimode".equals(args[0])) return uiModeService;
                if ("bluetooth_manager".equals(args[0])) {
                    return bluetoothManagerService;
                }
                if ("window".equals(args[0])) return windowService;
                if ("accessibility".equals(args[0])) return accessibilityService;
                if ("sensitive_content_protection_service".equals(args[0])) {
                    return sensitiveContentService;
                }
                if ("content".equals(args[0])) return contentService;
                if ("notification".equals(args[0])) return notificationService;
                if ("audio".equals(args[0])) return audioService;
                if ("voiceinteraction".equals(args[0])) {
                    return voiceInteractionService;
                }
                if ("search".equals(args[0])) return searchService;
                if ("clipboard".equals(args[0])) return clipboardService;
                if ("uri_grants".equals(args[0])) return uriGrantsService;
                if ("account".equals(args[0])) {
                    Log.i("DarwinServiceBridge", "ServiceManager account");
                    return accountService;
                }
                if ("mount".equals(args[0])) return storageService;
                if ("media_session".equals(args[0])) return mediaSessionService;
                if ("media_router".equals(args[0])) return mediaRouterService;
                if ("trust".equals(args[0])) return trustService;
                return missingService;
            }
            return defaultValue(method.getReturnType());
        }
    }

    /** Android's single primary physical user as exposed by system_server. */
    private static final class UserManagerHandler implements InvocationHandler {
        @Override
        public Object invoke(Object proxy, Method method, Object[] args) {
            String name = method.getName();
            if ("getProfileIds".equals(name) || "getEnabledProfileIds".equals(name)) {
                return new int[] {0};
            }
            if ("getApplicationRestrictionsForUser".equals(name)
                    || "getUserRestrictions".equals(name)) {
                return new Bundle();
            }
            if ("isUserUnlocked".equals(name) || "isUserRunning".equals(name)
                    || "isUserForeground".equals(name)) {
                return Boolean.TRUE;
            }
            if ("getUserSerialNumber".equals(name)) return Long.valueOf(0L);
            if ("getUserHandle".equals(name)) return Integer.valueOf(0);
            return defaultValue(method.getReturnType());
        }
    }

    /** Remote-style system_server endpoint for an unmanaged physical device. */
    private static final class DevicePolicyServiceBinder extends Binder {
        DevicePolicyServiceBinder() {
            attachInterface(null, "android.app.admin.IDevicePolicyManager");
        }

        @Override
        protected boolean onTransact(int code, android.os.Parcel data,
                android.os.Parcel reply, int flags) {
            if (reply != null) {
                reply.writeNoException();
                // Zero is a valid empty/false/null marker for the policy calls
                // consumed during app startup, including getActiveAdmins().
                reply.writeLong(0L);
            }
            return true;
        }
    }

    /** Minimal remote-style audio binder consumed by IAudioService.Stub.Proxy. */
    private static final class AudioServiceBinder extends Binder {
        private int nextPlayerId = 1;

        AudioServiceBinder() {
            attachInterface(null, "android.media.IAudioService");
        }

        @Override
        protected boolean onTransact(int code, android.os.Parcel data,
                android.os.Parcel reply, int flags) {
            if (reply != null) {
                reply.writeNoException();
                // trackPlayer is the first synchronous call made by
                // PlayerBase and expects a positive player id. Extra reply
                // data is ignored by void lifecycle transactions.
                reply.writeInt(nextPlayerId++);
            }
            return true;
        }
    }

    /** Process-local implementations of Android's media-session binder graph. */
    private static final class MediaSessionInterfaceHandler implements InvocationHandler {
        final Binder binder = new Binder();
        final Object proxy;
        private final String descriptor;
        private final String packageName;
        private Object controller;

        MediaSessionInterfaceHandler(String descriptor, String packageName) throws Exception {
            this.descriptor = descriptor;
            this.packageName = packageName;
            Class<?> interfaceClass = Class.forName(descriptor);
            proxy = Proxy.newProxyInstance(interfaceClass.getClassLoader(),
                    new Class<?>[] {interfaceClass}, this);
            attach(binder, proxy, descriptor);
        }

        @Override
        public Object invoke(Object ignored, Method method, Object[] args) throws Exception {
            String name = method.getName();
            if ("asBinder".equals(name)) return binder;
            if ("createSession".equals(name)
                    && "android.media.session.ISessionManager".equals(descriptor)) {
                String owner = args != null && args.length > 0 && args[0] instanceof String
                        ? (String) args[0] : packageName;
                return new MediaSessionInterfaceHandler(
                        "android.media.session.ISession", owner).proxy;
            }
            if ("getController".equals(name)
                    && "android.media.session.ISession".equals(descriptor)) {
                if (controller == null) {
                    controller = new MediaSessionInterfaceHandler(
                            "android.media.session.ISessionController", packageName).proxy;
                }
                return controller;
            }
            if ("getBinderForSetQueue".equals(name)) return new Binder();
            if ("getPackageName".equals(name)) return packageName;
            if ("isTransportControlEnabled".equals(name)) return Boolean.TRUE;
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
            if (method.getName().equals("getAppTasks")) {
                return new ArrayList<>();
            }
            if (method.getName().startsWith("startActivity")) {
                Intent intent = findArgument(args, Intent.class);
                Intent resolvedIntent = resolveChooserTarget(intent);
                ComponentName component = resolvedIntent == null
                        ? null : resolvedIntent.getComponent();
                Log.i("DarwinServiceBridge", "activity task " + method.getName()
                        + " action=" + (intent == null ? null : intent.getAction())
                        + " resolvedAction=" + (resolvedIntent == null
                                ? null : resolvedIntent.getAction())
                        + " component=" + component);
                String packageName = System.getenv("DARWIN_ART_APK_APP_PACKAGE");
                if (component != null && packageName != null
                        && packageName.equals(component.getPackageName())) {
                    Intent launchIntent = new Intent(resolvedIntent);
                    int requestCode = findRequestCode(method, args, intent);
                    MAIN_HANDLER.post(() -> launchLocalActivity(launchIntent, requestCode));
                    // ActivityManager.START_SUCCESS. The transition itself is
                    // asynchronous on Android as well, after the current
                    // lifecycle callback returns to the main Looper.
                    return Integer.valueOf(0);
                }
                if (isOpenDocumentIntent(resolvedIntent)) {
                    int requestCode = findRequestCode(method, args, intent);
                    deliverOpenDocumentResult(resolvedIntent, requestCode);
                    return Integer.valueOf(0);
                }
                if (isCreateDocumentIntent(resolvedIntent)) {
                    int requestCode = findRequestCode(method, args, intent);
                    deliverCreateDocumentResult(resolvedIntent, requestCode);
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

    private static Intent resolveChooserTarget(Intent intent) {
        if (intent == null || !Intent.ACTION_CHOOSER.equals(intent.getAction())) {
            return intent;
        }
        android.os.Parcelable target = intent.getParcelableExtra(Intent.EXTRA_INTENT);
        return target instanceof Intent ? (Intent) target : intent;
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
            Class<?> activityClass = Activity.class;
            ActivityInfo info;
            try {
                info = previous.getPackageManager().getActivityInfo(component, 0);
            } catch (Throwable ignored) {
                info = new ActivityInfo((ActivityInfo) getField(
                        activityClass, previous, "mActivityInfo"));
                info.name = component.getClassName();
            }
            String targetName = info.targetActivity == null
                    ? info.name : info.targetActivity;
            Class<?> targetClass = Class.forName(targetName, true, loader);
            Activity next = (Activity) targetClass.getDeclaredConstructor().newInstance();

            Object activityThread = getField(activityClass, previous, "mMainThread");
            Object instrumentation = getField(activityClass, previous, "mInstrumentation");
            Application application = previous.getApplication();
            Context baseContext = previous.getBaseContext();
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
            Method performDestroy = activityClass.getDeclaredMethod("performDestroy");
            performPause.setAccessible(true);
            performCreate.setAccessible(true);
            performStart.setAccessible(true);
            performResume.setAccessible(true);
            performStop.setAccessible(true);
            performDestroy.setAccessible(true);

            boolean previousFinishing = previous.isFinishing();
            if (!previousFinishing) performPause.invoke(previous);
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
            completeActivityWindowVisibility(next, decor);
            if (!nativeInstallActivity(next, decor)) {
                throw new IllegalStateException("native graphics Activity install failed");
            }
            currentActivity = next;
            ACTIVITY_STACK.add(new ActivityRecord(next, token, requestCode));
            performResume.invoke(next, Boolean.FALSE, "darwin-art activity launch");
            scheduleViewTreeDump(next, decor);
            // ActivityThread does not dispatch onStop() when an Activity calls
            // finish() from onCreate() before it ever reaches STARTED. Once the
            // replacement Activity is committed, keep cleanup failures isolated
            // from that transaction.
            try {
                if (previousFinishing) {
                    performDestroy.invoke(previous);
                } else {
                    performStop.invoke(previous, Boolean.FALSE,
                            "darwin-art activity launch");
                }
            } catch (Throwable cleanupError) {
                Log.w("DarwinServiceBridge",
                        "previous Activity lifecycle cleanup failed", cleanupError);
            }
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
            completeActivityWindowVisibility(caller.activity, decor);
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

    /** ActivityManager-owned token behind framework PendingIntent. */
    private static final class IntentSenderHandler implements InvocationHandler {
        final Binder binder = new Binder();
        final Object proxy;
        final int type;
        final Intent original;

        IntentSenderHandler(int type, Intent original) throws Exception {
            this.type = type;
            this.original = original == null ? null : new Intent(original);
            Class<?> senderInterface = Class.forName("android.content.IIntentSender");
            proxy = Proxy.newProxyInstance(senderInterface.getClassLoader(),
                    new Class<?>[] {senderInterface}, this);
            attach(binder, proxy, "android.content.IIntentSender");
        }

        @Override
        public Object invoke(Object ignored, Method method, Object[] args) {
            if ("asBinder".equals(method.getName())) return binder;
            if (method.getName().startsWith("send")) {
                deliver(findArgument(args, Intent.class));
            }
            return defaultValue(method.getReturnType());
        }

        void deliver(Intent fillIn) {
            Intent delivered = original == null ? null : new Intent(original);
            if (delivered != null && fillIn != null && fillIn != original) {
                delivered.fillIn(fillIn, 0);
            }
            Log.i("DarwinServiceBridge", "intent sender send type=" + type
                    + " component="
                    + (delivered == null ? null : delivered.getComponent()));
            // ActivityManager.INTENT_SENDER_ACTIVITY. Broadcast and service
            // senders remain separate system-server transactions.
            if (type == 2 && delivered != null) {
                Intent launchIntent = delivered;
                MAIN_HANDLER.post(() -> launchLocalActivity(launchIntent, -1));
            }
        }
    }

    /** Process-local ActivityManager state normally supplied by system_server. */
    private static final class ActivityManagerHandler implements InvocationHandler {
        @Override
        public Object invoke(Object proxy, Method method, Object[] args) throws Exception {
            if ("sendIntentSender".equals(method.getName())) {
                Class<?> senderInterface = Class.forName(
                        "android.content.IIntentSender");
                Object sender = findArgument(args, senderInterface);
                if (sender != null && Proxy.isProxyClass(sender.getClass())) {
                    InvocationHandler handler = Proxy.getInvocationHandler(sender);
                    if (handler instanceof IntentSenderHandler) {
                        ((IntentSenderHandler) handler).deliver(
                                findArgument(args, Intent.class));
                        // ActivityManager.START_SUCCESS.
                        return Integer.valueOf(0);
                    }
                }
                return Integer.valueOf(-1);
            }
            if (method.getName().startsWith("getIntentSender")) {
                // PendingIntent factories are backed by an IIntentSender token
                // allocated by ActivityManager on Android.  Keep that contract
                // process-local so framework PendingIntent APIs return a real,
                // stable binder object instead of null when system_server is
                // absent. Activity senders re-enter the same ActivityManager
                // transaction path used by ordinary startActivity calls.
                int type = 0;
                Intent original = null;
                if (args != null) {
                    for (Object argument : args) {
                        if (type == 0 && argument instanceof Integer) {
                            type = ((Integer) argument).intValue();
                        } else if (argument instanceof Intent[]
                                && ((Intent[]) argument).length > 0) {
                            Intent[] intents = (Intent[]) argument;
                            original = intents[intents.length - 1];
                        }
                    }
                }
                return new IntentSenderHandler(type, original).proxy;
            }
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
            if ("getAppTasks".equals(method.getName())) {
                // IActivityManager returns an empty list, never null, when the
                // caller owns no recent tasks. ActivityManager.getAppTasks()
                // iterates this result directly.
                return new ArrayList<>();
            }
            return defaultValue(method.getReturnType());
        }
    }

    private static final class DisplayHandler implements InvocationHandler {
        @Override
        public Object invoke(Object proxy, Method method, Object[] args) {
            if (method.getName().equals("getDisplayIds")) {
                return new int[] {0};
            }
            if (method.getName().equals("getDisplayInfo")) {
                Log.i("DarwinServiceBridge", "display getDisplayInfo");
                return displayInfo();
            }
            if (method.getName().equals("getWifiDisplayStatus")) {
                // Android always returns a status object, including on
                // devices without Wi-Fi display hardware. MediaRouter reads
                // it unconditionally while creating the local default route.
                try {
                    Class<?> status = Class.forName(
                            "android.hardware.display.WifiDisplayStatus");
                    Constructor<?> constructor = status.getDeclaredConstructor();
                    constructor.setAccessible(true);
                    return constructor.newInstance();
                } catch (ReflectiveOperationException error) {
                    throw new IllegalStateException(
                            "could not construct Wi-Fi display status", error);
                }
            }
            return defaultValue(method.getReturnType());
        }

        private static Object displayInfo() {
            try {
                Class<?> infoClass = Class.forName("android.view.DisplayInfo");
                Constructor<?> constructor = infoClass.getDeclaredConstructor();
                constructor.setAccessible(true);
                Object info = constructor.newInstance();
                // Populate the same minimum invariants DisplayManagerService
                // publishes for Android's built-in, powered-on display.  A
                // zero-initialized DisplayInfo is not a valid display: in
                // particular TYPE_UNKNOWN/STATE_UNKNOWN and a null identity
                // propagate into Chromium's cross-process ScreenInfos and can
                // make Widget.UpdateVisualProperties fail Mojo validation.
                put(infoClass, info, "displayId", Display.DEFAULT_DISPLAY);
                put(infoClass, info, "layerStack", Display.DEFAULT_DISPLAY);
                put(infoClass, info, "name", "Built-in Display");
                put(infoClass, info, "uniqueId", "local:darwin-art:0");
                put(infoClass, info, "type", DISPLAY_TYPE_INTERNAL);
                put(infoClass, info, "state", Display.STATE_ON);
                put(infoClass, info, "committedState", Display.STATE_ON);
                put(infoClass, info, "logicalWidth", DISPLAY_WIDTH);
                put(infoClass, info, "logicalHeight", DISPLAY_HEIGHT);
                put(infoClass, info, "appWidth", DISPLAY_WIDTH);
                put(infoClass, info, "appHeight", DISPLAY_HEIGHT);
                put(infoClass, info, "smallestNominalAppWidth", DISPLAY_WIDTH);
                put(infoClass, info, "smallestNominalAppHeight", DISPLAY_HEIGHT);
                put(infoClass, info, "largestNominalAppWidth", DISPLAY_WIDTH);
                put(infoClass, info, "largestNominalAppHeight", DISPLAY_HEIGHT);
                put(infoClass, info, "logicalDensityDpi", DISPLAY_DENSITY_DPI);
                put(infoClass, info, "physicalXDpi", (float) DISPLAY_DENSITY_DPI);
                put(infoClass, info, "physicalYDpi", (float) DISPLAY_DENSITY_DPI);
                Class<?> modeClass = Class.forName("android.view.Display$Mode");
                Constructor<?> modeConstructor = modeClass.getDeclaredConstructor(
                        int.class, int.class, int.class, float.class);
                modeConstructor.setAccessible(true);
                Object mode = modeConstructor.newInstance(
                        1, DISPLAY_WIDTH, DISPLAY_HEIGHT, 60.0f);
                Object modes = Array.newInstance(modeClass, 1);
                Array.set(modes, 0, mode);
                put(infoClass, info, "modeId", 1);
                put(infoClass, info, "defaultModeId", 1);
                put(infoClass, info, "supportedModes", modes);
                put(infoClass, info, "appsSupportedModes", modes);
                put(infoClass, info, "supportedRefreshRates", new float[] {60.0f});
                // Android 16 derives DisplayInfo.getRefreshRate() from these
                // fields (the old single refreshRate field no longer exists).
                put(infoClass, info, "refreshRateOverride", 60.0f);
                put(infoClass, info, "renderFrameRate", 60.0f);
                put(infoClass, info, "presentationDeadlineNanos", 16_666_666L);
                put(infoClass, info, "canHostTasks", true);
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
        private final Class<?> windowIdInterface;
        private final Map<Object, Object> windowIds = new HashMap<>();
        private final Map<Object, SurfaceControl> windowSurfaces = new HashMap<>();
        private final Map<Object, int[]> windowSurfaceSizes = new HashMap<>();
        private final Map<Object, WindowLayoutState> windowLayouts = new HashMap<>();
        private final ArrayList<Object> serverInputChannels = new ArrayList<>();

        private static final class WindowLayoutState {
            int x;
            int y;
            int width;
            int height;
            int type;
            int gravity;
            int flags;
        }

        WindowManagerHandler() throws ClassNotFoundException {
            Class<?> sessionInterface = Class.forName("android.view.IWindowSession");
            windowIdInterface = Class.forName("android.view.IWindowId");
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
                // WindowManagerGlobal.ADD_FLAG_APP_VISIBLE. ViewRootImpl uses
                // this WMS result to initialize mAppVisible; returning zero
                // makes every real traversal skip draw as view_not_visible.
                return Integer.valueOf(2);
            }
            if (proxy == session && "relayout".equals(method.getName())) {
                configureRelayout(args);
                // RELAYOUT_RES_FIRST_TIME: ViewRoot must complete its first
                // traversal even though CAMetalLayer owns the real surface.
                return Integer.valueOf(2);
            }
            if (proxy == session && "remove".equals(method.getName())) {
                Object windowToken = args == null || args.length == 0 ? null : args[0];
                SurfaceControl surface = windowSurfaces.remove(windowToken);
                if (surface != null && surface.isValid()) surface.release();
                windowSurfaceSizes.remove(windowToken);
                windowLayouts.remove(windowToken);
                windowIds.remove(windowToken);
                return defaultValue(method.getReturnType());
            }
            if (proxy == session && "getWindowId".equals(method.getName())) {
                Object token = args == null || args.length == 0 ? session : args[0];
                Object existing = windowIds.get(token);
                if (existing != null) return existing;
                Binder binder = new Binder();
                Object created = Proxy.newProxyInstance(windowIdInterface.getClassLoader(),
                        new Class<?>[] {windowIdInterface}, (idProxy, idMethod, idArgs) -> {
                            if ("asBinder".equals(idMethod.getName())) return binder;
                            if ("isFocused".equals(idMethod.getName())) return Boolean.TRUE;
                            return defaultValue(idMethod.getReturnType());
                        });
                windowIds.put(token, created);
                return created;
            }
            return defaultValue(method.getReturnType());
        }

        private void initializeAddOutputs(Object[] args) {
            if (args == null) return;
            Object windowToken = args.length == 0 ? null : args[0];
            for (Object value : args) {
                if (value != null && value.getClass().getName().equals(
                        "android.view.WindowManager$LayoutParams")) {
                    rememberLayout(windowToken, value);
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

        private void rememberLayout(Object windowToken, Object attrs) {
            if (windowToken == null || attrs == null) return;
            try {
                WindowLayoutState state = windowLayouts.computeIfAbsent(
                        windowToken, ignored -> new WindowLayoutState());
                state.x = intField(attrs, "x", state.x);
                state.y = intField(attrs, "y", state.y);
                state.width = intField(attrs, "width", state.width);
                state.height = intField(attrs, "height", state.height);
                state.type = intField(attrs, "type", state.type);
                state.gravity = intField(attrs, "gravity", state.gravity);
                state.flags = intField(attrs, "flags", state.flags);
            } catch (Throwable error) {
                Log.e("DarwinServiceBridge", "window layout state failed", error);
            }
        }

        private void configureRelayout(Object[] args) {
            if (args == null || args.length < 9 || args[8] == null) return;
            try {
                Object windowToken = args[0];
                Object attrs = args[1];
                if (attrs != null) rememberLayout(windowToken, attrs);
                WindowLayoutState state = windowLayouts.get(windowToken);
                int requestedWidth = ((Integer) args[2]).intValue();
                int requestedHeight = ((Integer) args[3]).intValue();
                int x = state == null ? 0 : state.x;
                int y = state == null ? 0 : state.y;
                int type = state == null ? 0 : state.type;
                int gravity = state == null ? 0 : state.gravity;
                int flags = state == null ? 0 : state.flags;
                int layoutWidth = state == null ? requestedWidth : state.width;
                int layoutHeight = state == null ? requestedHeight : state.height;
                int width = requestedWidth > 0 && requestedWidth <= DISPLAY_WIDTH
                        ? requestedWidth
                        : (layoutWidth > 0 && layoutWidth <= DISPLAY_WIDTH
                                ? layoutWidth : DISPLAY_WIDTH);
                int height = requestedHeight > 0 && requestedHeight <= DISPLAY_HEIGHT
                        ? requestedHeight
                        : (layoutHeight > 0 && layoutHeight <= DISPLAY_HEIGHT
                                ? layoutHeight : 120 * DISPLAY_SCALE);
                width = Math.min(width, DISPLAY_WIDTH);
                height = Math.min(height, DISPLAY_HEIGHT);
                // WMS fits an application sub-window into its display frame.
                // Clipping the buffer to the small remainder below an anchor
                // loses almost the entire popup; move the full window above
                // the edge instead, preserving its measured content size.
                x = Math.max(0, Math.min(x, DISPLAY_WIDTH - width));
                y = Math.max(0, Math.min(y, DISPLAY_HEIGHT - height));
                if (state != null) {
                    state.x = x;
                    state.y = y;
                    state.width = width;
                    state.height = height;
                }
                if (System.getenv("DARWIN_ART_DEBUG_WINDOW_LAYERS") != null) {
                    Log.i("DarwinServiceBridge", "window frame request="
                            + requestedWidth + "x" + requestedHeight
                            + " layout=" + layoutWidth + "x" + layoutHeight
                            + " output=" + width + "x" + height + " at=" + x + "," + y
                            + " type=" + type + " gravity=0x"
                            + Integer.toHexString(gravity) + " flags=0x"
                            + Integer.toHexString(flags));
                }

                Object result = args[8];
                Field framesField = result.getClass().getField("frames");
                Object frames = framesField.get(result);
                setRectField(frames, "frame", x, y, x + width, y + height);
                setRectField(frames, "displayFrame", 0, 0,
                        DISPLAY_WIDTH, DISPLAY_HEIGHT);
                setRectField(frames, "parentFrame", 0, 0,
                        DISPLAY_WIDTH, DISPLAY_HEIGHT);
                SurfaceControl producer = windowSurfaces.get(windowToken);
                int[] producerSize = windowSurfaceSizes.get(windowToken);
                if (producer == null || !producer.isValid()
                        || producerSize == null || producerSize[0] != width
                        || producerSize[1] != height) {
                    if (producer != null && producer.isValid()) producer.release();
                    producer = new SurfaceControl.Builder()
                            .setName("Darwin ART ViewRoot")
                            .setBufferSize(width, height)
                            .build();
                    windowSurfaces.put(windowToken, producer);
                    windowSurfaceSizes.put(windowToken, new int[] {width, height});
                }
                new SurfaceControl.Transaction()
                        .setPosition(producer, x, y)
                        .setLayer(producer, type >= 1000 ? 1000 : 0)
                        .apply();
                Field surfaceControlField = result.getClass().getField("surfaceControl");
                SurfaceControl output =
                        (SurfaceControl) surfaceControlField.get(result);
                Method copyFrom = SurfaceControl.class.getDeclaredMethod(
                        "copyFrom", SurfaceControl.class, String.class);
                copyFrom.setAccessible(true);
                copyFrom.invoke(output, producer,
                        "DarwinServiceBridge.relayout");
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

    private static Object emptyFrameworkContainer(Class<?> type, String methodName) {
        try {
            Method empty = type.getDeclaredMethod(methodName);
            empty.setAccessible(true);
            return empty.invoke(null);
        } catch (ReflectiveOperationException error) {
            throw new IllegalStateException(
                    "could not create empty framework container " + type.getName(), error);
        }
    }
}
