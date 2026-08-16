package dev.darwinart.simple;

import android.app.Activity;
import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.view.Gravity;
import android.widget.Button;
import android.widget.FrameLayout;

/** A no-resource, no-native launcher Activity backed by a real Android widget. */
public final class MainActivity extends Activity {
    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        FontBootstrap.install();
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(0xff0f172a);
        Button button = new Button(this);
        button.setText("Android Button");
        button.setAllCaps(false);
        button.setGravity(Gravity.CENTER);
        button.setTextColor(0xffffffff);
        button.setTextSize(20);
        GradientDrawable background = new GradientDrawable();
        background.setColor(0xff2563eb);
        background.setCornerRadius(24);
        button.setBackground(background);
        FrameLayout.LayoutParams params = new FrameLayout.LayoutParams(320, 96);
        params.gravity = Gravity.CENTER;
        root.addView(button, params);
        setContentView(root);
    }
}
