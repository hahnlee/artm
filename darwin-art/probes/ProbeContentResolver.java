package dev.darwinart.probe;

import android.content.ContentResolver;
import android.content.Context;
import android.content.IContentProvider;
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
