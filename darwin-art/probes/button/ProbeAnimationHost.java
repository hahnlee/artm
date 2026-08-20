package dev.darwinart.probe;

import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.ArrayList;

/** Creates the hidden RenderNode.AnimationHost without hidden compile stubs. */
public final class ProbeAnimationHost {
    private ProbeAnimationHost() {}

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
}
