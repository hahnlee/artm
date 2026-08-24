package dev.darwinart.probe;

import android.content.ComponentName;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.test.mock.MockPackageManager;

/** Minimal package policy for framework feature gates before system_server exists. */
public final class ProbePackageManager extends MockPackageManager {
    private String packageName;
    private String activityName;
    private ActivityInfo activityInfo;

    public ProbePackageManager() {}

    /** Installs metadata for the unmodified APK selected by the native launcher. */
    public void configure(String packageName, String activityName, ActivityInfo source,
            int targetSdkVersion) {
        this.packageName = packageName;
        this.activityName = activityName;
        activityInfo = source == null ? new ActivityInfo() : new ActivityInfo(source);
        activityInfo.packageName = packageName;
        activityInfo.name = activityName;
        if (activityInfo.applicationInfo == null) {
            activityInfo.applicationInfo = new ApplicationInfo();
        }
        activityInfo.applicationInfo.packageName = packageName;
        activityInfo.applicationInfo.targetSdkVersion = targetSdkVersion;
    }

    @Override
    public ActivityInfo getActivityInfo(ComponentName component, int flags)
            throws PackageManager.NameNotFoundException {
        if (component != null
                && packageName != null
                && packageName.equals(component.getPackageName())
                && activityName != null
                && activityName.equals(component.getClassName())) {
            return new ActivityInfo(activityInfo);
        }
        throw new PackageManager.NameNotFoundException(String.valueOf(component));
    }

    @Override
    public ApplicationInfo getApplicationInfo(String requestedPackage, int flags)
            throws PackageManager.NameNotFoundException {
        if (packageName != null && packageName.equals(requestedPackage)) {
            return new ApplicationInfo(activityInfo.applicationInfo);
        }
        throw new PackageManager.NameNotFoundException(requestedPackage);
    }

    @Override
    public boolean hasSystemFeature(String name) {
        return false;
    }

    @Override
    public boolean hasSystemFeature(String name, int version) {
        return false;
    }
}
