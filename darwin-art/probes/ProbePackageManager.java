package dev.darwinart.probe;

import android.test.mock.MockPackageManager;

/** Minimal package policy for framework feature gates before system_server exists. */
public final class ProbePackageManager extends MockPackageManager {
    public ProbePackageManager() {}

    @Override
    public boolean hasSystemFeature(String name) {
        return false;
    }

    @Override
    public boolean hasSystemFeature(String name, int version) {
        return false;
    }
}
