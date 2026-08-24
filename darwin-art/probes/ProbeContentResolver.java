package dev.darwinart.probe;

import android.content.ContentResolver;
import android.content.Context;
import android.content.IContentProvider;
import android.database.MatrixCursor;
import android.os.Bundle;

import java.lang.reflect.Proxy;

final class ProbeContentResolver extends ContentResolver {
    private final IContentProvider settingsProvider;

    ProbeContentResolver(Context context) {
        super(context);
        settingsProvider = (IContentProvider) Proxy.newProxyInstance(
                ProbeContentResolver.class.getClassLoader(),
                new Class<?>[] { IContentProvider.class },
                (proxy, method, args) -> {
                    if (method.getName().equals("call")) {
                        Bundle result = new Bundle();
                        result.putString("value", "1");
                        return result;
                    }
                    if (method.getName().equals("query")) {
                        String[] projection = null;
                        if (args != null) {
                            for (Object arg : args) {
                                if (arg instanceof String[]) {
                                    projection = (String[]) arg;
                                    break;
                                }
                            }
                        }
                        // A provider with no persisted rows returns an empty
                        // cursor, never null. Framework loaders and unchanged
                        // APKs rely on the cursor contract even when the host
                        // has no Android alarm/settings database yet.
                        return new MatrixCursor(
                                projection == null ? new String[0] : projection);
                    }
                    Class<?> returnType = method.getReturnType();
                    if (returnType == boolean.class) return false;
                    if (returnType == int.class) return 0;
                    if (returnType == long.class) return 0L;
                    return null;
                });
    }

    protected IContentProvider acquireProvider(Context context, String name) {
        return settingsProvider;
    }

    protected IContentProvider acquireUnstableProvider(Context context, String name) {
        return settingsProvider;
    }

    public boolean releaseProvider(IContentProvider provider) {
        return true;
    }

    public boolean releaseUnstableProvider(IContentProvider provider) {
        return true;
    }

    public void unstableProviderDied(IContentProvider provider) {}
}
