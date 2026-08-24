package android.media;

import android.content.Context;

/**
 * Process-local audio service used when the Darwin runtime has no Android
 * system_server. UI sound effects must remain harmless, while the framework
 * still receives a correctly typed AudioManager from Context.
 */
public final class ProbeAudioManager extends AudioManager {
    public ProbeAudioManager(Context context) {
        // The public SDK surface exposes only the no-argument constructor;
        // the host intentionally has no ContextImpl-backed audio service.
        super();
    }

    // This public framework method is hidden from the SDK stub used by javac,
    // so intentionally omit @Override. It overrides the real Android 16
    // virtual method when this class is loaded by ART.
    public boolean areNavigationRepeatSoundEffectsEnabled() {
        return false;
    }

    @Override
    public void playSoundEffect(int effectType) {}

    @Override
    public void playSoundEffect(int effectType, float volume) {}
}
