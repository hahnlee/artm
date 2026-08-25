package dev.darwinart.security;

import java.security.Provider;

/** Boot-class-path JCA provider for a detached Darwin Android process. */
public final class DarwinSecurityProvider extends Provider {
    public DarwinSecurityProvider() {
        super("DarwinART", 1.0, "Darwin ART host entropy provider");
        put("SecureRandom.SHA1PRNG", DarwinSecureRandom.class.getName());
        put("SecureRandom.NativePRNG", DarwinSecureRandom.class.getName());
    }
}
