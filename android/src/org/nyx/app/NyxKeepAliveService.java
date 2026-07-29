package org.nyx.app;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;
import android.util.Log;

public final class NyxKeepAliveService extends Service {
    private static final String TAG = "NyxKeepAlive";
    private static final String CHANNEL_ID = "nyx_keepalive_v1";
    private static final int NOTIF_ID = 7201;

    public static void start(Context ctx) {
        if (ctx == null) return;
        try {
            final Context app = ctx.getApplicationContext();
            Intent i = new Intent(app, NyxKeepAliveService.class);
            Log.i(TAG, "start requested sdk=" + Build.VERSION.SDK_INT);
            if (Build.VERSION.SDK_INT >= 26) {
                app.startForegroundService(i);
            } else {
                app.startService(i);
            }
        } catch (Throwable t) {
            Log.e(TAG, "start failed", t);
        }
    }

    public static void stop(Context ctx) {
        if (ctx == null) return;
        try {
            final Context app = ctx.getApplicationContext();
            Log.i(TAG, "stop requested");
            app.stopService(new Intent(app, NyxKeepAliveService.class));
        } catch (Throwable t) {
            Log.e(TAG, "stop failed", t);
        }
    }

    @Override
    public void onCreate() {
        super.onCreate();
        ensureChannel();
        try {
            Notification n = buildNotification();
            if (Build.VERSION.SDK_INT >= 34) {
                startForeground(NOTIF_ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC);
            } else {
                startForeground(NOTIF_ID, n);
            }
            Log.i(TAG, "foreground started");
        } catch (Throwable t) {
            Log.e(TAG, "startForeground failed", t);
            stopSelf();
        }
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        Log.i(TAG, "destroyed");
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    private void ensureChannel() {
        if (Build.VERSION.SDK_INT < 26) return;
        NotificationManager nm = getSystemService(NotificationManager.class);
        if (nm == null) return;
        NotificationChannel ch = new NotificationChannel(
                CHANNEL_ID, "Nyx в сети", NotificationManager.IMPORTANCE_LOW);
        ch.setDescription("Держит соединение для входящих звонков");
        ch.setShowBadge(false);
        ch.enableVibration(false);
        ch.setSound(null, null);
        nm.createNotificationChannel(ch);
    }

    private Notification buildNotification() {
        Intent open = new Intent(this, org.qtproject.qt.android.bindings.QtActivity.class);
        open.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP
                | Intent.FLAG_ACTIVITY_REORDER_TO_FRONT);
        int flags = PendingIntent.FLAG_UPDATE_CURRENT;
        if (Build.VERSION.SDK_INT >= 23) flags |= PendingIntent.FLAG_IMMUTABLE;
        PendingIntent pi = PendingIntent.getActivity(this, 2, open, flags);

        Notification.Builder b;
        if (Build.VERSION.SDK_INT >= 26) {
            b = new Notification.Builder(this, CHANNEL_ID);
        } else {
            b = new Notification.Builder(this);
        }
        return b.setContentTitle("Nyx")
                .setContentText("В сети — ждут входящие звонки")
                .setSmallIcon(android.R.drawable.ic_menu_info_details)
                .setContentIntent(pi)
                .setOngoing(true)
                .setCategory(Notification.CATEGORY_SERVICE)
                .setPriority(Notification.PRIORITY_MIN)
                .build();
    }
}
