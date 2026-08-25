package android.net;

import android.content.Context;
import android.os.Handler;

/** Process-local connectivity service for the Darwin Android framework port. */
public class ConnectivityManager {
    public static final int TYPE_MOBILE = 0;
    public static final int TYPE_WIFI = 1;

    private final NetworkInfo activeNetwork =
            new NetworkInfo(TYPE_WIFI, true);

    public ConnectivityManager(Context context) {}

    public NetworkInfo getActiveNetworkInfo() {
        return activeNetwork;
    }

    public Network[] getAllNetworks() {
        return new Network[] {new Network(1)};
    }

    public NetworkCapabilities getNetworkCapabilities(Network network) {
        return network == null ? null : new NetworkCapabilities(false);
    }

    public void registerDefaultNetworkCallback(NetworkCallback callback) {
        if (callback != null) callback.onAvailable(activeNetworkHandle());
    }

    public void registerDefaultNetworkCallback(NetworkCallback callback, Handler handler) {
        if (callback == null) return;
        Runnable available = () -> callback.onAvailable(activeNetworkHandle());
        if (handler == null) available.run(); else handler.post(available);
    }

    public void unregisterNetworkCallback(NetworkCallback callback) {}

    private Network activeNetworkHandle() { return new Network(1); }

    /** Android-compatible callback surface used by connectivity-aware apps. */
    public static class NetworkCallback {
        public NetworkCallback() {}
        public NetworkCallback(int flags) {}
        public void onAvailable(Network network) {}
        public void onLosing(Network network, int maxMsToLive) {}
        public void onLost(Network network) {}
        public void onUnavailable() {}
        public void onCapabilitiesChanged(Network network,
                NetworkCapabilities capabilities) {}
        public void onNetworkSuspended(Network network) {}
        public void onNetworkResumed(Network network) {}
        public void onBlockedStatusChanged(Network network, boolean blocked) {}
    }
}
