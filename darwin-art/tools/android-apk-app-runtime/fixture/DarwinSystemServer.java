package dev.darwinart.system;

import android.os.Binder;
import android.os.Parcel;

/** Persistent ART owner for Darwin ART's first cross-process system service. */
public final class DarwinSystemServer {
    public static final String DESCRIPTOR = "dev.darwinart.system.IPackageRegistry";
    public static final int TRANSACTION_RESOLVE_PACKAGE = 1;

    private DarwinSystemServer() {}

    private static native String nativeResolvePackage(String packageName);

    public static Binder createPackageRegistry() {
        return new PackageRegistryBinder();
    }

    private static final class PackageRegistryBinder extends Binder {
        PackageRegistryBinder() {
            attachInterface(null, DESCRIPTOR);
        }

        @Override
        protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) {
            if (code != TRANSACTION_RESOLVE_PACKAGE || reply == null) return false;
            data.enforceInterface(DESCRIPTOR);
            String packageName = data.readString();
            String record = nativeResolvePackage(packageName);
            reply.writeNoException();
            reply.writeString(record);
            return true;
        }
    }
}
