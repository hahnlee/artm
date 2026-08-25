package android.net;

/** Transport properties exposed by the process-local connectivity service. */
public final class NetworkCapabilities {
    public static final int TRANSPORT_VPN = 4;

    private final boolean vpn;

    public NetworkCapabilities(boolean vpn) {
        this.vpn = vpn;
    }

    public boolean hasTransport(int transportType) {
        return transportType == TRANSPORT_VPN && vpn;
    }
}
