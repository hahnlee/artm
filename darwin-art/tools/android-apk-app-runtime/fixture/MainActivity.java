package dev.darwinart.simple;

import android.app.Activity;
import android.os.Bundle;
import android.graphics.drawable.GradientDrawable;
import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.Paint;
import android.graphics.PixelFormat;
import android.graphics.drawable.Drawable;
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
        Thread.currentThread().setContextClassLoader(MainActivity.class.getClassLoader());
        FontBootstrap.install();
        String stage = "LinearLayout";
        try {
            LinearLayout root = new LinearLayout(this);
            root.setOrientation(LinearLayout.VERTICAL);
            root.setPadding(32, 20, 32, 20);
            // Material You light-scheme surface tones.  The platform Material
            // theme still owns typography, state lists, ripples, and widget
            // metrics; these values only keep this no-resource fixture tidy.
            root.setBackgroundColor(0xfffffbfe);

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
            checkBox.setTextColor(0xff1d1b20);
            checkBox.setChecked(true);
            toggles.addView(checkBox, weighted());

            stage = "RadioButton";
            RadioButton radio = new RadioButton(this);
            radio.setTag("radio");
            radio.setText("RadioButton");
            radio.setTextColor(0xff1d1b20);
            radio.setChecked(true);
            toggles.addView(radio, weighted());
            root.addView(toggles, matchWidth());

            stage = "ToggleButton";
            ToggleButton toggle = new ToggleButton(this);
            toggle.setTag("toggle");
            toggle.setTextOn("Toggle on");
            toggle.setTextOff("Toggle off");
            toggle.setTextColor(0xff1d1b20);
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
            button.setTextColor(0xffffffff);
            button.setAllCaps(false);
            GradientDrawable buttonSurface = new GradientDrawable();
            buttonSurface.setColor(0xff6750a4);
            buttonSurface.setCornerRadius(14f);
            // Android 16's patterned RippleDrawable is GPU-only. The Darwin
            // host intentionally rasterizes a software Canvas, so use this
            // tiny Material-compatible software ripple that still advances
            // with Android's real ValueAnimator/Choreographer clock.
            MaterialRippleDrawable ripple = new MaterialRippleDrawable(buttonSurface);
            button.setBackground(ripple);
            button.setOnTouchListener((view, event) -> {
                if (event.getAction() == 0) {
                    ripple.start(event.getX(), event.getY());
                }
                return false;
            });
            button.setOnClickListener(view -> button.setText("Button pressed"));
            root.addView(button, matchWidth());

            stage = "setContentView";
            setContentView(root);
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

    private static final class MaterialRippleDrawable extends Drawable {
        private final Drawable content;
        private final Paint ripplePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private float progress;
        private float hotspotX;
        private float hotspotY;
        private long startedNanos;
        private boolean pressed;

        MaterialRippleDrawable(Drawable content) {
            this.content = content;
            ripplePaint.setColor(0x33ffffff);
        }

        void start(float x, float y) {
            hotspotX = x;
            hotspotY = y;
            pressed = true;
            startedNanos = System.nanoTime();
            progress = 0.0f;
            invalidateSelf();
        }

        @Override
        public void draw(Canvas canvas) {
            content.setBounds(getBounds());
            content.draw(canvas);
            if (pressed) {
                progress = Math.min(1.0f,
                        (System.nanoTime() - startedNanos) / 360_000_000.0f);
                if (progress < 1.0f) invalidateSelf();
            }
            if (progress <= 0.0f) return;
            float radius = (float) Math.hypot(getBounds().width(), getBounds().height());
            ripplePaint.setAlpha(Math.max(0, Math.round(51.0f * (1.0f - progress))));
            canvas.drawCircle(hotspotX, hotspotY, radius * progress, ripplePaint);
        }

        @Override
        protected boolean onStateChange(int[] state) {
            boolean pressed = false;
            for (int value : state) {
                if (value == android.R.attr.state_pressed) {
                    pressed = true;
                    break;
                }
            }
            if (pressed) {
                this.pressed = true;
                startedNanos = System.nanoTime();
                progress = 0.0f;
                invalidateSelf();
            } else if (this.pressed) {
                this.pressed = false;
                invalidateSelf();
            }
            return true;
        }

        @Override
        public boolean isStateful() {
            return true;
        }

        @Override
        public void setHotspot(float x, float y) {
            hotspotX = x;
            hotspotY = y;
            invalidateSelf();
        }

        @Override
        protected void onBoundsChange(android.graphics.Rect bounds) {
            content.setBounds(bounds);
        }

        @Override
        public void setAlpha(int alpha) {
            content.setAlpha(alpha);
        }

        @Override
        public void setColorFilter(ColorFilter filter) {
            content.setColorFilter(filter);
        }

        @Override
        public int getOpacity() {
            return PixelFormat.TRANSLUCENT;
        }
    }

}
