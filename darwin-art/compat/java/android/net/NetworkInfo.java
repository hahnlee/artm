package android.net;

/** Current connectivity snapshot for legacy CONNECTIVITY_ACTION clients. */
public final class NetworkInfo {
    private final int type;
    private final boolean connected;

    public NetworkInfo(int type, boolean connected) {
        this.type = type;
        this.connected = connected;
    }

    public boolean isConnected() {
        return connected;
    }

    public int getType() {
        return type;
    }
}
