package dev.darwinart.probe;

import android.content.Context;
import android.view.View;
import android.view.ViewGroup;

/** Programmatic android.R.id.content parent inside the real DecorView. */
public final class ProbeContentRoot extends ViewGroup {
    public ProbeContentRoot(Context context) {
        super(context);
        setId(android.R.id.content);
    }

    @Override
    protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        int width = MeasureSpec.getSize(widthMeasureSpec);
        int height = MeasureSpec.getSize(heightMeasureSpec);
        for (int index = 0; index < getChildCount(); index++) {
            View child = getChildAt(index);
            child.measure(
                    MeasureSpec.makeMeasureSpec(width, MeasureSpec.EXACTLY),
                    MeasureSpec.makeMeasureSpec(height, MeasureSpec.EXACTLY));
        }
        setMeasuredDimension(width, height);
    }

    @Override
    protected void onLayout(boolean changed, int left, int top, int right, int bottom) {
        int width = right - left;
        int height = bottom - top;
        for (int index = 0; index < getChildCount(); index++) {
            getChildAt(index).layout(0, 0, width, height);
        }
    }
}
