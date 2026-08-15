package dev.darwinart.probe;

import android.content.ContextWrapper;
import android.content.pm.ApplicationInfo;

public final class ProbeContext extends ContextWrapper {
    private final ApplicationInfo applicationInfo;

    public ProbeContext() {
        super(null);
        applicationInfo = new ApplicationInfo();
        applicationInfo.targetSdkVersion = 36;
    }

    @Override
    public ApplicationInfo getApplicationInfo() {
        return applicationInfo;
    }

    @Override
    public String getPackageName() {
        return "dev.darwinart.probe";
    }

    @Override
    public String getOpPackageName() {
        return getPackageName();
    }
}
