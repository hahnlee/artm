package android.telephony;

import android.content.Context;

/**
 * Process-local telephony facade for the detached Android application runtime.
 *
 * The Darwin host has no modem or telephony system_server, but Android
 * applications still expect this framework object to exist when collecting
 * device metadata.  Keep the public contract deterministic and return the
 * same empty values Android uses when no SIM/operator is available.
 */
public final class TelephonyManager {
    public static final int PHONE_TYPE_NONE = 0;
    private final Context context;

    public TelephonyManager(Context context) {
        this.context = context;
    }

    public String getNetworkOperatorName() {
        return "";
    }

    public String getSimOperator() {
        return "";
    }

    public String getSimOperatorName() {
        return "";
    }

    public String getNetworkCountryIso() {
        return "";
    }

    public String getSimCountryIso() {
        return "";
    }

    public String getNetworkOperator() {
        return "";
    }

    public int getPhoneType() {
        return PHONE_TYPE_NONE;
    }

    public Context getContext() {
        return context;
    }
}
