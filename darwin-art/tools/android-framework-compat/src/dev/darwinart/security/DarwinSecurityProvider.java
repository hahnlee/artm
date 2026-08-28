package dev.darwinart.security;

import java.security.Provider;

/** Boot-class-path JCA provider for a detached Darwin Android process. */
public final class DarwinSecurityProvider extends Provider {
    public DarwinSecurityProvider() {
        super("DarwinART", 1.0, "Darwin ART macOS security provider");
        put("SecureRandom.SHA1PRNG", DarwinSecureRandom.class.getName());
        put("SecureRandom.NativePRNG", DarwinSecureRandom.class.getName());
        put("TrustManagerFactory.PKIX", DarwinTrustManagerFactory.class.getName());
        put("Alg.Alias.TrustManagerFactory.X509", "PKIX");
        put("Alg.Alias.TrustManagerFactory.SunX509", "PKIX");
        put("KeyStore.AndroidCAStore", DarwinAndroidCAStore.class.getName());
        DarwinAndroidCAStore.preload();
        if (System.getenv("DARWIN_ART_DEBUG_SECURITY") != null) {
            System.err.println("DARWIN security: provider initialized");
        }
    }
}
