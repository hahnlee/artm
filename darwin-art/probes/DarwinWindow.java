package dev.darwinart.probe;

import android.content.Context;
import android.content.res.Configuration;
import android.graphics.drawable.Drawable;
import android.net.Uri;
import android.os.Bundle;
import android.view.InputQueue;
import android.view.KeyEvent;
import android.view.LayoutInflater;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;

/** Android Window policy whose top-level surface is an independent NSWindow. */
public final class DarwinWindow extends Window {
    private static final int WIDTH = 640;
    private static final int HEIGHT = 360;

    private View content;
    private int navigationBarColor;
    private int statusBarColor;
    private int volumeControlStream;

    public DarwinWindow(Context context) {
        super(context);
    }

    private static native boolean presentContent(View view, int width, int height);

    public View getContent() {
        return content;
    }

    @Override
    public void setContentView(View view) {
        setContentView(view, new ViewGroup.LayoutParams(WIDTH, HEIGHT));
    }

    @Override
    public void setContentView(View view, ViewGroup.LayoutParams params) {
        content = view;
        if (!presentContent(view, WIDTH, HEIGHT)) {
            throw new IllegalStateException("Darwin host could not present Activity content");
        }
        Callback callback = getCallback();
        if (callback != null) {
            callback.onContentChanged();
        }
    }

    @Override
    public void setContentView(int layoutResID) {
        throw new UnsupportedOperationException("layout resources are not enabled yet");
    }

    @Override
    public void addContentView(View view, ViewGroup.LayoutParams params) {
        setContentView(view, params);
    }

    @Override
    public View getDecorView() {
        return content;
    }

    @Override
    public View peekDecorView() {
        return content;
    }

    @Override
    public View getCurrentFocus() {
        return content != null && content.hasFocus() ? content : null;
    }

    @Override
    public LayoutInflater getLayoutInflater() {
        return null;
    }

    @Override
    public void takeSurface(SurfaceHolder.Callback2 callback) {}

    @Override
    public void takeInputQueue(InputQueue.Callback callback) {}

    @Override
    public boolean isFloating() {
        return false;
    }

    @Override
    public void setTitle(CharSequence title) {}

    @Override
    public void setTitleColor(int textColor) {}

    @Override
    public void openPanel(int featureId, KeyEvent event) {}

    @Override
    public void closePanel(int featureId) {}

    @Override
    public void togglePanel(int featureId, KeyEvent event) {}

    @Override
    public void invalidatePanelMenu(int featureId) {}

    @Override
    public boolean performPanelShortcut(int featureId, int keyCode, KeyEvent event, int flags) {
        return false;
    }

    @Override
    public boolean performPanelIdentifierAction(int featureId, int id, int flags) {
        return false;
    }

    @Override
    public void closeAllPanels() {}

    @Override
    public boolean performContextMenuIdentifierAction(int id, int flags) {
        return false;
    }

    @Override
    public void onConfigurationChanged(Configuration newConfig) {}

    @Override
    public void setBackgroundDrawable(Drawable drawable) {}

    @Override
    public void setFeatureDrawableResource(int featureId, int resId) {}

    @Override
    public void setFeatureDrawableUri(int featureId, Uri uri) {}

    @Override
    public void setFeatureDrawable(int featureId, Drawable drawable) {}

    @Override
    public void setFeatureDrawableAlpha(int featureId, int alpha) {}

    @Override
    public void setFeatureInt(int featureId, int value) {}

    @Override
    public void takeKeyEvents(boolean get) {}

    @Override
    public boolean superDispatchKeyEvent(KeyEvent event) {
        return content != null && content.dispatchKeyEvent(event);
    }

    @Override
    public boolean superDispatchKeyShortcutEvent(KeyEvent event) {
        return content != null && content.dispatchKeyShortcutEvent(event);
    }

    @Override
    public boolean superDispatchTouchEvent(MotionEvent event) {
        return content != null && content.dispatchTouchEvent(event);
    }

    @Override
    public boolean superDispatchTrackballEvent(MotionEvent event) {
        return content != null && content.dispatchTrackballEvent(event);
    }

    @Override
    public boolean superDispatchGenericMotionEvent(MotionEvent event) {
        return content != null && content.dispatchGenericMotionEvent(event);
    }

    @Override
    public Bundle saveHierarchyState() {
        return new Bundle();
    }

    @Override
    public void restoreHierarchyState(Bundle savedInstanceState) {}

    @Override
    protected void onActive() {}

    @Override
    public void setChildDrawable(int featureId, Drawable drawable) {}

    @Override
    public void setChildInt(int featureId, int value) {}

    @Override
    public boolean isShortcutKey(int keyCode, KeyEvent event) {
        return false;
    }

    @Override
    public void setVolumeControlStream(int streamType) {
        volumeControlStream = streamType;
    }

    @Override
    public int getVolumeControlStream() {
        return volumeControlStream;
    }

    @Override
    public void setStatusBarColor(int color) {
        statusBarColor = color;
    }

    @Override
    public int getStatusBarColor() {
        return statusBarColor;
    }

    @Override
    public void setNavigationBarColor(int color) {
        navigationBarColor = color;
    }

    @Override
    public int getNavigationBarColor() {
        return navigationBarColor;
    }

    @Override
    public void setDecorCaptionShade(int decorCaptionShade) {}

    @Override
    public void setResizingCaptionDrawable(Drawable drawable) {}
}
