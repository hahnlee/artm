package dev.darwinart.simple;

import android.app.Activity;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.os.Bundle;
import android.view.View;

/** A no-resource, no-native launcher Activity used by the real APK gate. */
public final class MainActivity extends Activity {
    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        setContentView(new AppView(this));
    }

    private static final class AppView extends View {
        private static final int WIDTH = 640;
        private static final int HEIGHT = 360;

        AppView(Context context) {
            super(context);
        }

        @Override
        protected void onDraw(Canvas canvas) {
            if (canvas.getClass() != Canvas.class) {
                int[] pixels = new int[WIDTH * HEIGHT];
                fill(pixels, 0, 0, WIDTH, HEIGHT, 0xff0f172a);
                fill(pixels, 36, 36, WIDTH - 72, HEIGHT - 72, 0xfff8fafc);
                fill(pixels, 36, 36, WIDTH - 72, 74, 0xff3ddc84);
                fill(pixels, 82, 154, WIDTH - 164, 22, 0xff334155);
                fill(pixels, 82, 196, WIDTH - 248, 18, 0xff94a3b8);
                fill(pixels, 214, 274, 212, 50, 0xff2563eb);
                canvas.drawBitmap(pixels, 0, WIDTH, 0, 0, WIDTH, HEIGHT, true, null);
                return;
            }
            Paint paint = new Paint();
            canvas.drawColor(0xff0f172a);
            rect(canvas, paint, 36, 36, WIDTH - 72, HEIGHT - 72, 0xfff8fafc);
            rect(canvas, paint, 36, 36, WIDTH - 72, 74, 0xff3ddc84);
            rect(canvas, paint, 82, 154, WIDTH - 164, 22, 0xff334155);
            rect(canvas, paint, 82, 196, WIDTH - 248, 18, 0xff94a3b8);
            rect(canvas, paint, 214, 274, 212, 50, 0xff2563eb);
        }

        private static void rect(
                Canvas canvas, Paint paint, int left, int top, int width, int height, int color) {
            paint.setColor(color);
            canvas.drawRect(left, top, left + width, top + height, paint);
        }

        private static void fill(
                int[] pixels, int left, int top, int width, int height, int color) {
            for (int y = top; y < top + height; y++) {
                int row = y * WIDTH;
                for (int x = left; x < left + width; x++) {
                    pixels[row + x] = color;
                }
            }
        }
    }
}
