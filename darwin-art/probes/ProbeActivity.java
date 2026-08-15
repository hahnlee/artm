package dev.darwinart.probe;

import android.app.Activity;
import android.os.Bundle;

public final class ProbeActivity extends Activity {
    private int lifecycleValue;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        lifecycleValue = 43;
    }

    public int probeOnCreate() {
        onCreate(null);
        return lifecycleValue;
    }

    public int probeValue() {
        return 42;
    }
}
