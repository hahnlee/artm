package dev.darwinart.simple;

import android.app.Activity;
import android.os.Bundle;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.RadioButton;
import android.widget.SeekBar;
import android.widget.TextView;
import android.widget.ToggleButton;

/** A no-resource, no-native launcher Activity backed by Android framework widgets. */
public final class MainActivity extends Activity {
    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        FontBootstrap.install();
        String stage = "LinearLayout";
        try {
            LinearLayout root = new LinearLayout(this);
            root.setOrientation(LinearLayout.VERTICAL);
            root.setPadding(32, 20, 32, 20);
            root.setBackgroundColor(
                    getResources().getColor(android.R.color.background_light, getTheme()));

            stage = "TextView";
            TextView title = new TextView(this);
            title.setTag("title");
            title.setText("Android framework widgets");
            root.addView(title, matchWidth());

            stage = "CheckBox";
            LinearLayout toggles = new LinearLayout(this);
            toggles.setOrientation(LinearLayout.HORIZONTAL);
            CheckBox checkBox = new CheckBox(this);
            checkBox.setTag("checkbox");
            checkBox.setText("CheckBox");
            checkBox.setChecked(true);
            toggles.addView(checkBox, weighted());

            stage = "RadioButton";
            RadioButton radio = new RadioButton(this);
            radio.setTag("radio");
            radio.setText("RadioButton");
            radio.setChecked(true);
            toggles.addView(radio, weighted());
            root.addView(toggles, matchWidth());

            stage = "ToggleButton";
            ToggleButton toggle = new ToggleButton(this);
            toggle.setTag("toggle");
            toggle.setTextOn("Toggle on");
            toggle.setTextOff("Toggle off");
            root.addView(toggle, matchWidth());

            stage = "SeekBar";
            SeekBar seek = new SeekBar(this);
            seek.setTag("seek");
            seek.setMax(100);
            seek.setProgress(42);
            root.addView(seek, matchWidth());

            stage = "ProgressBar";
            ProgressBar progress = new ProgressBar(
                    this, null, android.R.attr.progressBarStyleHorizontal);
            progress.setTag("progress");
            progress.setMax(100);
            progress.setProgress(68);
            root.addView(progress, matchWidth());

            stage = "Button";
            Button button = new Button(this);
            button.setTag("button");
            button.setText("Default Android Button");
            root.addView(button, matchWidth());

            stage = "setContentView";
            setContentView(root);
            freezeDetachedAnimations(root);
        } catch (Throwable error) {
            throw new IllegalStateException("Default widget failed: " + stage, error);
        }
    }

    private static LinearLayout.LayoutParams matchWidth() {
        return new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
    }

    private static LinearLayout.LayoutParams weighted() {
        return new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1);
    }

    private static void freezeDetachedAnimations(View view) {
        view.setStateListAnimator(null);
        view.jumpDrawablesToCurrentState();
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            for (int index = 0; index < group.getChildCount(); index++) {
                freezeDetachedAnimations(group.getChildAt(index));
            }
        }
    }
}
