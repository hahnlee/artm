package android.net;

/** Stable process-local identity for a Darwin host network. */
public final class Network {
    private final int netId;

    public Network(int netId) {
        this.netId = netId;
    }

    public int getNetId() {
        return netId;
    }

    @Override
    public int hashCode() {
        return netId;
    }

    @Override
    public boolean equals(Object other) {
        return other instanceof Network && ((Network) other).netId == netId;
    }
}
