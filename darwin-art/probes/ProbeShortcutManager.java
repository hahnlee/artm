package android.content.pm;

import java.util.List;

/** In-process shortcut service client for a detached Android app process. */
public final class ProbeShortcutManager extends ShortcutManager {
    public ProbeShortcutManager() {
        super();
    }

    @Override
    public void reportShortcutUsed(String shortcutId) {
        // system_server normally records this usage signal. The standalone
        // host has no launcher shortcut database, so accepting it is enough.
    }

    @Override
    public boolean setDynamicShortcuts(List<ShortcutInfo> shortcutInfoList) {
        return true;
    }

    @Override
    public boolean updateShortcuts(List<ShortcutInfo> shortcutInfoList) {
        return true;
    }
}
