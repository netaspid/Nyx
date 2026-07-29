package org.nyx.app;

import android.app.Activity;
import android.content.Context;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;

import java.util.HashSet;
import java.util.Set;

public final class NyxCameraSurfaces {
    private static final String TAG = "NyxCameraSurfaces";
    private static final Handler MAIN = new Handler(Looper.getMainLooper());
    private static final Set<Integer> BASELINE = new HashSet<>();

    private NyxCameraSurfaces() {}


    public static void markBaseline(Context ctx) {
        final Activity act = activityOf(ctx);
        if (act == null) return;
        Runnable r = () -> {
            BASELINE.clear();
            View root = decor(act);
            if (!(root instanceof ViewGroup)) return;
            collectIds((ViewGroup) root, BASELINE);
            Log.i(TAG, "baseline surfaces=" + BASELINE.size());
        };
        if (Looper.myLooper() == Looper.getMainLooper()) r.run();
        else MAIN.post(r);
    }


    public static void suppressOverlays(Context ctx) {
        final Activity act = activityOf(ctx);
        if (act == null) return;
        MAIN.post(() -> suppressNow(act));
        MAIN.postDelayed(() -> suppressNow(act), 200);
        MAIN.postDelayed(() -> suppressNow(act), 500);
        MAIN.postDelayed(() -> suppressNow(act), 1200);
        MAIN.postDelayed(() -> suppressNow(act), 2500);
    }

    private static void suppressNow(Activity act) {
        try {
            if (act.isFinishing()) return;
            View root = decor(act);
            if (!(root instanceof ViewGroup)) return;
            int n = demoteNew((ViewGroup) root);
            if (n > 0) Log.i(TAG, "demoted " + n + " camera SurfaceView(s)");
        } catch (Throwable t) {
            Log.e(TAG, "suppress failed", t);
        }
    }

    private static int demoteNew(ViewGroup group) {
        int demoted = 0;
        for (int i = 0; i < group.getChildCount(); ++i) {
            View child = group.getChildAt(i);
            if (child instanceof SurfaceView) {
                final int id = System.identityHashCode(child);
                if (!BASELINE.contains(id)) {
                    SurfaceView sv = (SurfaceView) child;
                    try {
                        sv.setZOrderOnTop(false);
                        sv.setZOrderMediaOverlay(false);
                        sv.setClickable(false);
                        sv.setFocusable(false);
                        sv.setFocusableInTouchMode(false);
                        sv.setAlpha(0f);
                        demoted++;
                    } catch (Throwable t) {
                        Log.w(TAG, "demote failed", t);
                    }
                }
            }
            if (child instanceof ViewGroup) demoted += demoteNew((ViewGroup) child);
        }
        return demoted;
    }

    private static void collectIds(ViewGroup group, Set<Integer> out) {
        for (int i = 0; i < group.getChildCount(); ++i) {
            View child = group.getChildAt(i);
            if (child instanceof SurfaceView) out.add(System.identityHashCode(child));
            if (child instanceof ViewGroup) collectIds((ViewGroup) child, out);
        }
    }

    private static Activity activityOf(Context ctx) {
        return (ctx instanceof Activity) ? (Activity) ctx : null;
    }

    private static View decor(Activity act) {
        return act.getWindow() != null ? act.getWindow().getDecorView() : null;
    }
}
