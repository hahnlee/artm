package dev.darwinart.probe;

import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.FeatureInfo;
import android.content.pm.PackageManager;
import android.content.pm.PackageInfo;
import android.content.pm.ProviderInfo;
import android.content.pm.ResolveInfo;
import android.content.pm.ServiceInfo;
import android.content.res.Resources;
import android.graphics.Color;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.Drawable;
import android.os.Bundle;
import android.test.mock.MockPackageManager;
import android.util.TypedValue;
import java.util.HashMap;
import java.util.Collections;
import java.util.List;
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
    private Map<String, String> activityAliases;

    public ProbePackageManager() {}

    @Override
    public boolean isPermissionRevokedByPolicy(
            String permission, String packageName) {
        // Runtime permission state is owned by this PackageManager facade;
        // device-policy revocation is false until a DevicePolicyManager
        // explicitly publishes such a restriction.
        return false;
    }

    public boolean shouldShowRequestPermissionRationale(String permission) {
        // No permission request has been denied with "ask again" state in the
        // detached package manager. Android reports false for this initial
        // state; a future permission-controller service can persist denials.
        return false;
    }

    static void applyApplicationPaths(ApplicationInfo info) {
        if (info == null) return;
        String apk = System.getenv("DARWIN_ART_APK_APP_RESOURCE_APK");
        if (apk != null && !apk.isEmpty()) {
            info.sourceDir = apk;
            info.publicSourceDir = apk;
        }
        String data = System.getenv("DARWIN_ART_APK_APP_DATA_GUEST_DIR");
        if (data != null && !data.isEmpty()) info.dataDir = data;
    }

    static void applyApplicationMetadata(ApplicationInfo info, Resources resources) {
        String encoded = System.getenv("DARWIN_ART_APK_APP_METADATA");
        if (info == null || encoded == null || encoded.isEmpty()
                || "none".equals(encoded)) return;
        Bundle metadata = new Bundle();
        for (String entry : encoded.split(",")) {
            String[] fields = entry.split(":", -1);
            if (fields.length != 3 || fields[1].length() != 1) continue;
            String name = decodeHexString(fields[0]);
            if (name == null) continue;
            try {
                switch (fields[1].charAt(0)) {
                    case 'r': {
                        int resource = (int) Long.parseLong(fields[2], 16);
                        TypedValue value = new TypedValue();
                        resources.getValue(resource, value, true);
                        if (value.type == TypedValue.TYPE_STRING) {
                            metadata.putString(name, String.valueOf(value.string));
                        } else if (value.type == TypedValue.TYPE_INT_BOOLEAN) {
                            metadata.putBoolean(name, value.data != 0);
                        } else if (value.type >= TypedValue.TYPE_FIRST_INT
                                && value.type <= TypedValue.TYPE_LAST_INT) {
                            metadata.putInt(name, value.data);
                        } else {
                            metadata.putInt(name, resource);
                        }
                        break;
                    }
                    case 'i':
                        metadata.putInt(name, (int) Long.parseLong(fields[2], 16));
                        break;
                    case 'b':
                        metadata.putBoolean(name, "1".equals(fields[2]));
                        break;
                    case 's': {
                        String value = decodeHexString(fields[2]);
                        if (value != null) metadata.putString(name, value);
                        break;
                    }
                    default:
                        break;
                }
            } catch (RuntimeException ignored) {
                // PackageParser skips malformed/unsupported metadata values;
                // preserve the remaining manifest entries.
            }
        }
        info.metaData = metadata;
    }

    private static String decodeHexString(String encoded) {
        if ((encoded.length() & 1) != 0) return null;
        byte[] bytes = new byte[encoded.length() / 2];
        for (int index = 0; index < bytes.length; index++) {
            int high = Character.digit(encoded.charAt(index * 2), 16);
            int low = Character.digit(encoded.charAt(index * 2 + 1), 16);
            if (high < 0 || low < 0) return null;
            bytes[index] = (byte) ((high << 4) | low);
        }
        try {
            return new String(bytes, "UTF-8");
        } catch (java.io.UnsupportedEncodingException impossible) {
            throw new AssertionError(impossible);
        }
    }

    @Override
    public String[] getPackagesForUid(int uid) {
        // This runtime currently hosts exactly one installed Android package
        // per process, matching PackageManager's caller-identity check used by
        // MediaBrowserService and other Binder services.
        return packageName == null ? null : new String[] {packageName};
    }

    @Override
    public String getNameForUid(int uid) {
        return packageName;
    }

    @Override
    public List<ResolveInfo> queryBroadcastReceivers(Intent intent, int flags) {
        return Collections.emptyList();
    }

    @Override
    public List<ResolveInfo> queryBroadcastReceivers(Intent intent,
            PackageManager.ResolveInfoFlags flags) {
        return Collections.emptyList();
    }

    @Override
    public List<ResolveInfo> queryIntentServices(Intent intent, int flags) {
        return Collections.emptyList();
    }

    @Override
    public List<ResolveInfo> queryIntentServices(Intent intent,
            PackageManager.ResolveInfoFlags flags) {
        return Collections.emptyList();
    }

    @Override
    public ResolveInfo resolveService(Intent intent, int flags) {
        return null;
    }

    @Override
    public ResolveInfo resolveService(Intent intent, PackageManager.ResolveInfoFlags flags) {
        return null;
    }

    @Override
    public ProviderInfo resolveContentProvider(String authority, int flags) {
        return null;
    }

    @Override
    public ProviderInfo resolveContentProvider(String authority,
            PackageManager.ComponentInfoFlags flags) {
        return null;
    }

    @Override
    public List<ProviderInfo> queryContentProviders(
            String processName, int uid, int flags) {
        return Collections.emptyList();
    }

    @Override
    public List<ResolveInfo> queryIntentContentProviders(Intent intent, int flags) {
        return Collections.emptyList();
    }

    @Override
    public List<ResolveInfo> queryIntentContentProviders(Intent intent,
            PackageManager.ResolveInfoFlags flags) {
        return Collections.emptyList();
    }

    /** Installs metadata for the unmodified APK selected by the native launcher. */
    public void configure(String packageName, String activityName, String activityNames,
            String activityAliasNames, String serviceNames,
            ActivityInfo source, int targetSdkVersion, Resources resources,
            int versionCode, String versionName) {
        componentEnabledSettings = new HashMap<>();
        applicationEnabledSettings = new HashMap<>();
        serviceInfos = new HashMap<>();
        activityThemes = new HashMap<>();
        activityAliases = new HashMap<>();
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
        configured.applicationInfo.nativeLibraryDir = System.getenv(
                "DARWIN_ART_APK_APP_NATIVE_DIR");
        applyApplicationPaths(configured.applicationInfo);
        applyApplicationMetadata(configured.applicationInfo, resources);
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
        if (activityAliasNames != null && !"none".equals(activityAliasNames)) {
            for (String entry : activityAliasNames.split(",")) {
                int separator = entry.indexOf('>');
                if (separator <= 0 || separator == entry.length() - 1) continue;
                activityAliases.put(entry.substring(0, separator),
                        entry.substring(separator + 1));
            }
        }
        if (serviceNames != null && !"none".equals(serviceNames)) {
            for (String serviceSpec : serviceNames.split(",")) {
                int separator = serviceSpec.indexOf('>');
                String serviceName = separator < 0
                        ? serviceSpec : serviceSpec.substring(0, separator);
                String processName = separator < 0
                        ? packageName : serviceSpec.substring(separator + 1);
                if (serviceName.isEmpty() || processName.isEmpty()) continue;
                ServiceInfo service = new ServiceInfo();
                service.packageName = packageName;
                service.name = serviceName;
                service.processName = processName;
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
    public ServiceInfo getServiceInfo(ComponentName component,
            PackageManager.ComponentInfoFlags flags)
            throws PackageManager.NameNotFoundException {
        return getServiceInfo(component, (int) flags.getValue());
    }

    @Override
    public ActivityInfo getActivityInfo(ComponentName component, int flags)
            throws PackageManager.NameNotFoundException {
        String requestedName = component == null ? null : component.getClassName();
        String targetName = requestedName == null ? null : activityAliases.get(requestedName);
        String resolvedName = targetName == null ? requestedName : targetName;
        if (component != null
                && packageName != null
                && packageName.equals(component.getPackageName())
                && activityThemes.containsKey(resolvedName)) {
            ActivityInfo result = new ActivityInfo(activityInfo);
            result.name = requestedName;
            result.targetActivity = targetName;
            result.theme = activityThemes.get(resolvedName).intValue();
            return result;
        }
        throw new PackageManager.NameNotFoundException(String.valueOf(component));
    }

    @Override
    public ActivityInfo getActivityInfo(ComponentName component,
            PackageManager.ComponentInfoFlags flags)
            throws PackageManager.NameNotFoundException {
        return getActivityInfo(component, (int) flags.getValue());
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
    public ApplicationInfo getApplicationInfo(String requestedPackage,
            PackageManager.ApplicationInfoFlags flags)
            throws PackageManager.NameNotFoundException {
        return getApplicationInfo(requestedPackage, (int) flags.getValue());
    }

    @Override
    public String getInstallerPackageName(String requestedPackage) {
        return null;
    }

    @Override
    public CharSequence getApplicationLabel(ApplicationInfo info) {
        if (info == null || packageName == null || !packageName.equals(info.packageName)) {
            throw new IllegalArgumentException("Unknown application");
        }
        String configured = System.getenv("DARWIN_ART_APK_APP_LABEL");
        return configured == null || configured.isEmpty() ? packageName : configured;
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
        info.activities = new ActivityInfo[activityThemes.size() + activityAliases.size()];
        int index = 0;
        for (Map.Entry<String, Integer> entry : activityThemes.entrySet()) {
            ActivityInfo declared = new ActivityInfo(activityInfo);
            declared.name = entry.getKey();
            declared.theme = entry.getValue().intValue();
            info.activities[index++] = declared;
        }
        for (Map.Entry<String, String> entry : activityAliases.entrySet()) {
            ActivityInfo declared = new ActivityInfo(activityInfo);
            declared.name = entry.getKey();
            declared.targetActivity = entry.getValue();
            Integer targetTheme = activityThemes.get(entry.getValue());
            declared.theme = targetTheme == null ? 0 : targetTheme.intValue();
            info.activities[index++] = declared;
        }
        return info;
    }

    @Override
    public PackageInfo getPackageInfo(String requestedPackage,
            PackageManager.PackageInfoFlags flags)
            throws PackageManager.NameNotFoundException {
        return getPackageInfo(requestedPackage, (int) flags.getValue());
    }

    @Override
    public ResolveInfo resolveActivity(Intent intent, int flags) {
        if (intent == null || intent.getComponent() == null) {
            return null;
        }
        ComponentName component = intent.getComponent();
        String requestedName = component.getClassName();
        String targetName = activityAliases.get(requestedName);
        String resolvedName = targetName == null ? requestedName : targetName;
        if (packageName == null || !packageName.equals(component.getPackageName())
                || !activityThemes.containsKey(resolvedName)) {
            return null;
        }
        ResolveInfo resolved = new ResolveInfo();
        resolved.activityInfo = new ActivityInfo(activityInfo);
        resolved.activityInfo.name = requestedName;
        resolved.activityInfo.targetActivity = targetName;
        resolved.activityInfo.theme = activityThemes.get(resolvedName).intValue();
        return resolved;
    }

    @Override
    public List<ResolveInfo> queryIntentActivities(Intent intent, int flags) {
        ResolveInfo resolved = resolveActivity(intent, flags);
        return resolved == null
                ? Collections.emptyList() : Collections.singletonList(resolved);
    }

    @Override
    public List<ResolveInfo> queryIntentActivities(Intent intent,
            PackageManager.ResolveInfoFlags flags) {
        return queryIntentActivities(intent, (int) flags.getValue());
    }

    @Override
    public boolean hasSystemFeature(String name) {
        return PackageManager.FEATURE_TOUCHSCREEN.equals(name);
    }

    @Override
    public boolean hasSystemFeature(String name, int version) {
        return hasSystemFeature(name);
    }

    @Override
    public FeatureInfo[] getSystemAvailableFeatures() {
        FeatureInfo touchscreen = new FeatureInfo();
        touchscreen.name = PackageManager.FEATURE_TOUCHSCREEN;
        return new FeatureInfo[] {touchscreen};
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
