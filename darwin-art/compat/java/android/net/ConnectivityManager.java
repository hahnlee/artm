package android.net;

import android.content.Context;
import android.os.Handler;

import java.util.concurrent.CopyOnWriteArrayList;

/** Process-local connectivity service for the Darwin Android framework port. */
public class ConnectivityManager {
    public static final int TYPE_MOBILE = 0;
    public static final int TYPE_WIFI = 1;

    private final NetworkInfo activeNetwork =
            new NetworkInfo(TYPE_WIFI, true);
    private final CopyOnWriteArrayList<OnNetworkActiveListener> networkActiveListeners =
            new CopyOnWriteArrayList<>();

    public ConnectivityManager(Context context) {}

    public NetworkInfo getActiveNetworkInfo() {
        return activeNetwork;
    }

    /** The host's primary Ethernet/Wi-Fi route is treated as unmetered. */
    public boolean isActiveNetworkMetered() {
        return false;
    }

    public Network[] getAllNetworks() {
        return new Network[] {new Network(1)};
    }

    public Network getActiveNetwork() {
        return activeNetworkHandle();
    }

    public NetworkInfo getNetworkInfo(Network network) {
        return network == null ? null : activeNetwork;
    }

    public NetworkInfo getNetworkInfo(int networkType) {
        return networkType == TYPE_WIFI ? activeNetwork : null;
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

    public void registerNetworkCallback(NetworkRequest request, NetworkCallback callback) {
        registerNetworkCallback(request, callback, null);
    }

    public void registerNetworkCallback(
            NetworkRequest request, NetworkCallback callback, Handler handler) {
        if (request == null) throw new NullPointerException("request");
        if (callback == null) throw new NullPointerException("callback");
        Network network = activeNetworkHandle();
        NetworkCapabilities capabilities = getNetworkCapabilities(network);
        if (!request.canBeSatisfiedBy(capabilities)) return;
        Runnable available = () -> {
            callback.onAvailable(network);
            callback.onCapabilitiesChanged(network, capabilities);
        };
        if (handler == null) available.run(); else handler.post(available);
    }

    public void unregisterNetworkCallback(NetworkCallback callback) {}

    public boolean isDefaultNetworkActive() {
        return true;
    }

    public void addDefaultNetworkActiveListener(OnNetworkActiveListener listener) {
        if (listener == null) throw new IllegalArgumentException("listener is null");
        networkActiveListeners.addIfAbsent(listener);
    }

    public void removeDefaultNetworkActiveListener(OnNetworkActiveListener listener) {
        if (listener == null) throw new IllegalArgumentException("listener is null");
        networkActiveListeners.remove(listener);
    }

    private Network activeNetworkHandle() { return new Network(1); }

    /** Listener notified when the system default network has active traffic. */
    public interface OnNetworkActiveListener {
        void onNetworkActive();
    }

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
