package dev.darwinart.probe;

import android.content.Context;
import android.graphics.Canvas;
import android.widget.Button;

/** Isolated real-widget acceptance class loaded in place of the baseline ProbeView. */
public final class ProbeView extends Button {
    private boolean presented;

    public ProbeView(Context context) {
        super(context);
        setText("Click");
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        presented = true;
    }

    public boolean wasPresented() {
        return presented;
    }
}
