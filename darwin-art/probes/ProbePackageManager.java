package dev.darwinart.probe;

import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.pm.PackageInfo;
import android.content.pm.ResolveInfo;
import android.content.pm.ServiceInfo;
import android.content.res.Resources;
import android.graphics.Color;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.Drawable;
import android.test.mock.MockPackageManager;
import java.util.HashMap;
import java.util.Map;

/** Minimal package policy for framework feature gates before system_server exists. */
public final class ProbePackageManager extends MockPackageManager {
    private String packageName;
    private String activityName;
    private ActivityInfo activityInfo;
    private Resources resources;
    private int versionCode;
    private String versionName;
    private Map<ComponentName, Integer> componentEnabledSettings;
    private Map<String, Integer> applicationEnabledSettings;
    private Map<ComponentName, ServiceInfo> serviceInfos;
    private Map<String, Integer> activityThemes;

    public ProbePackageManager() {}

    /** Installs metadata for the unmodified APK selected by the native launcher. */
    public void configure(String packageName, String activityName, String activityNames,
            String serviceNames,
            ActivityInfo source, int targetSdkVersion, Resources resources,
            int versionCode, String versionName) {
        componentEnabledSettings = new HashMap<>();
        applicationEnabledSettings = new HashMap<>();
        serviceInfos = new HashMap<>();
        activityThemes = new HashMap<>();
        this.packageName = packageName;
        this.activityName = activityName;
        ActivityInfo configured = source == null ? new ActivityInfo() : source;
        configured.packageName = packageName;
        configured.name = activityName;
        if (configured.applicationInfo == null) {
            configured.applicationInfo = new ApplicationInfo();
        }
        configured.applicationInfo.packageName = packageName;
        configured.applicationInfo.targetSdkVersion = targetSdkVersion;
        activityInfo = new ActivityInfo(configured);
        if (activityNames != null && !"none".equals(activityNames)) {
            for (String entry : activityNames.split(",")) {
                int separator = entry.lastIndexOf('=');
                if (separator <= 0 || separator == entry.length() - 1) continue;
                String name = entry.substring(0, separator);
                int theme = Long.decode(entry.substring(separator + 1)).intValue();
                activityThemes.put(name, Integer.valueOf(theme));
            }
        }
        if (serviceNames != null && !"none".equals(serviceNames)) {
            for (String serviceName : serviceNames.split(",")) {
                if (serviceName.isEmpty()) continue;
                ServiceInfo service = new ServiceInfo();
                service.packageName = packageName;
                service.name = serviceName;
                service.applicationInfo = new ApplicationInfo(activityInfo.applicationInfo);
                serviceInfos.put(new ComponentName(packageName, serviceName), service);
            }
        }
        this.resources = resources;
        this.versionCode = versionCode;
        this.versionName = versionName;
    }

    @Override
    public ServiceInfo getServiceInfo(ComponentName component, int flags)
            throws PackageManager.NameNotFoundException {
        ServiceInfo service = serviceInfos.get(component);
        if (service != null) return new ServiceInfo(service);
        throw new PackageManager.NameNotFoundException(String.valueOf(component));
    }

    @Override
    public ActivityInfo getActivityInfo(ComponentName component, int flags)
            throws PackageManager.NameNotFoundException {
        if (component != null
                && packageName != null
                && packageName.equals(component.getPackageName())
                && activityThemes.containsKey(component.getClassName())) {
            ActivityInfo result = new ActivityInfo(activityInfo);
            result.name = component.getClassName();
            result.theme = activityThemes.get(component.getClassName()).intValue();
            return result;
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
    public PackageInfo getPackageInfo(String requestedPackage, int flags)
            throws PackageManager.NameNotFoundException {
        if (packageName == null || !packageName.equals(requestedPackage)) {
            throw new PackageManager.NameNotFoundException(requestedPackage);
        }
        PackageInfo info = new PackageInfo();
        info.packageName = packageName;
        info.applicationInfo = new ApplicationInfo(activityInfo.applicationInfo);
        info.versionCode = versionCode;
        info.setLongVersionCode(versionCode & 0xffff_ffffL);
        info.versionName = versionName;
        info.activities = new ActivityInfo[activityThemes.size()];
        int index = 0;
        for (Map.Entry<String, Integer> entry : activityThemes.entrySet()) {
            ActivityInfo declared = new ActivityInfo(activityInfo);
            declared.name = entry.getKey();
            declared.theme = entry.getValue().intValue();
            info.activities[index++] = declared;
        }
        return info;
    }

    @Override
    public ResolveInfo resolveActivity(Intent intent, int flags) {
        if (intent == null || intent.getComponent() == null) {
            return null;
        }
        ComponentName component = intent.getComponent();
        if (packageName == null || !packageName.equals(component.getPackageName())
                || !activityThemes.containsKey(component.getClassName())) {
            return null;
        }
        ResolveInfo resolved = new ResolveInfo();
        resolved.activityInfo = new ActivityInfo(activityInfo);
        resolved.activityInfo.name = component.getClassName();
        resolved.activityInfo.theme = activityThemes.get(component.getClassName()).intValue();
        return resolved;
    }

    @Override
    public boolean hasSystemFeature(String name) {
        return false;
    }

    @Override
    public boolean hasSystemFeature(String name, int version) {
        return false;
    }

    @Override
    public int getComponentEnabledSetting(ComponentName component) {
        Integer setting = componentEnabledSettings.get(component);
        return setting == null
                ? PackageManager.COMPONENT_ENABLED_STATE_DEFAULT
                : setting.intValue();
    }

    @Override
    public void setComponentEnabledSetting(ComponentName component, int newState, int flags) {
        if (component == null) {
            throw new IllegalArgumentException("component must not be null");
        }
        componentEnabledSettings.put(component, Integer.valueOf(newState));
    }

    @Override
    public int getApplicationEnabledSetting(String requestedPackage) {
        if (packageName == null || !packageName.equals(requestedPackage)) {
            throw new IllegalArgumentException("Unknown package: " + requestedPackage);
        }
        Integer setting = applicationEnabledSettings.get(requestedPackage);
        return setting == null
                ? PackageManager.COMPONENT_ENABLED_STATE_DEFAULT
                : setting.intValue();
    }

    @Override
    public void setApplicationEnabledSetting(String requestedPackage, int newState, int flags) {
        if (packageName == null || !packageName.equals(requestedPackage)) {
            throw new IllegalArgumentException("Unknown package: " + requestedPackage);
        }
        applicationEnabledSettings.put(requestedPackage, Integer.valueOf(newState));
    }

    @Override
    public Drawable getDefaultActivityIcon() {
        if (resources != null) {
            try {
                return resources.getDrawable(android.R.drawable.sym_def_app_icon, null);
            } catch (Resources.NotFoundException ignored) {
                // A transparent framework Drawable still preserves the
                // PhoneWindow icon contract when a reduced resource table has
                // no symbolic default icon.
            }
        }
        return new ColorDrawable(Color.TRANSPARENT);
    }

    @Override
    public Drawable getDrawable(String requestedPackage, int resourceId,
            ApplicationInfo applicationInfo) {
        if (resources == null || resourceId == 0
                || packageName == null || !packageName.equals(requestedPackage)) {
            return null;
        }
        try {
            return resources.getDrawable(resourceId, null);
        } catch (Resources.NotFoundException ignored) {
            return null;
        }
    }
}
