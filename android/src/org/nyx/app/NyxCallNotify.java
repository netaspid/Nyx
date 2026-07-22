package org.nyx.app;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.media.AudioAttributes;
import android.media.AudioManager;
import android.media.RingtoneManager;
import android.net.Uri;
import android.os.Build;
import android.os.PowerManager;

/** Local high-priority notification for incoming Nyx calls (process must be alive). */
public final class NyxCallNotify {
    private static final String CHANNEL_ID = "nyx_calls_v2";
    private static final int NOTIF_INCOMING = 7101;
    private static final int NOTIF_ACTIVE = 7102;
    private static PowerManager.WakeLock sWakeLock;

    private NyxCallNotify() {}

    public static void ensureChannel(Context ctx) {
        if (Build.VERSION.SDK_INT < 26) return;
        NotificationManager nm = ctx.getSystemService(NotificationManager.class);
        if (nm == null) return;
        // Delete old silent channel if present.
        try { nm.deleteNotificationChannel("nyx_calls"); } catch (Exception ignored) {}
        NotificationChannel ch = new NotificationChannel(
                CHANNEL_ID, "Звонки Nyx", NotificationManager.IMPORTANCE_HIGH);
        ch.setDescription("Входящие и активные звонки");
        ch.enableVibration(true);
        ch.setVibrationPattern(new long[]{0, 400, 200, 400, 200, 400});
        ch.setLockscreenVisibility(Notification.VISIBILITY_PUBLIC);
        ch.setBypassDnd(true);
        Uri ringtone = RingtoneManager.getDefaultUri(RingtoneManager.TYPE_RINGTONE);
        AudioAttributes aa = new AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_NOTIFICATION_RINGTONE)
                .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                .build();
        ch.setSound(ringtone, aa);
        nm.createNotificationChannel(ch);
    }

    private static PendingIntent openAppIntent(Context ctx) {
        Intent open = new Intent(ctx, org.qtproject.qt.android.bindings.QtActivity.class);
        open.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP
                | Intent.FLAG_ACTIVITY_REORDER_TO_FRONT | Intent.FLAG_ACTIVITY_CLEAR_TOP);
        open.putExtra("nyx_incoming_call", true);
        int flags = PendingIntent.FLAG_UPDATE_CURRENT;
        if (Build.VERSION.SDK_INT >= 23) flags |= PendingIntent.FLAG_IMMUTABLE;
        return PendingIntent.getActivity(ctx, 1, open, flags);
    }

    private static Notification.Builder builder(Context ctx) {
        ensureChannel(ctx);
        if (Build.VERSION.SDK_INT >= 26)
            return new Notification.Builder(ctx, CHANNEL_ID);
        return new Notification.Builder(ctx);
    }

    public static void acquireWakeLock(Context ctx) {
        try {
            PowerManager pm = (PowerManager) ctx.getSystemService(Context.POWER_SERVICE);
            if (pm == null) return;
            if (sWakeLock == null) {
                sWakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "nyx:call");
                sWakeLock.setReferenceCounted(false);
            }
            if (!sWakeLock.isHeld()) sWakeLock.acquire(60_000L);
        } catch (Exception ignored) {}
    }

    public static void releaseWakeLock() {
        try {
            if (sWakeLock != null && sWakeLock.isHeld()) sWakeLock.release();
        } catch (Exception ignored) {}
    }

    public static void setVoipAudioMode(Context ctx, boolean active) {
        try {
            AudioManager am = (AudioManager) ctx.getSystemService(Context.AUDIO_SERVICE);
            if (am == null) return;
            if (active) {
                am.setMode(AudioManager.MODE_IN_COMMUNICATION);
                am.setSpeakerphoneOn(true);
                am.setMicrophoneMute(false);
            } else {
                am.setMode(AudioManager.MODE_NORMAL);
            }
        } catch (Exception ignored) {}
    }

    public static void showIncoming(Context ctx, String title, String body) {
        acquireWakeLock(ctx);
        PendingIntent pi = openAppIntent(ctx);
        Uri ringtone = RingtoneManager.getDefaultUri(RingtoneManager.TYPE_RINGTONE);
        Notification.Builder b = builder(ctx)
                .setContentTitle(title != null && title.length() > 0 ? title : "Nyx")
                .setContentText(body != null ? body : "Входящий звонок")
                .setSmallIcon(android.R.drawable.stat_sys_phone_call)
                .setContentIntent(pi)
                .setOngoing(true)
                .setAutoCancel(false)
                .setOnlyAlertOnce(false)
                .setCategory(Notification.CATEGORY_CALL)
                .setPriority(Notification.PRIORITY_MAX)
                .setVisibility(Notification.VISIBILITY_PUBLIC)
                .setSound(ringtone)
                .setVibrate(new long[]{0, 400, 200, 400, 200, 400});
        if (Build.VERSION.SDK_INT >= 21) {
            b.setFullScreenIntent(pi, true);
        }
        NotificationManager nm = (NotificationManager) ctx.getSystemService(Context.NOTIFICATION_SERVICE);
        if (nm != null) nm.notify(NOTIF_INCOMING, b.build());
    }

    public static void showActive(Context ctx, String title, String body) {
        PendingIntent pi = openAppIntent(ctx);
        Notification.Builder b = builder(ctx)
                .setContentTitle(title != null && title.length() > 0 ? title : "Nyx")
                .setContentText(body != null ? body : "Звонок")
                .setSmallIcon(android.R.drawable.stat_sys_phone_call)
                .setContentIntent(pi)
                .setOngoing(true)
                .setAutoCancel(false)
                .setCategory(Notification.CATEGORY_CALL)
                .setPriority(Notification.PRIORITY_DEFAULT);
        if (Build.VERSION.SDK_INT >= 26) {
            // Ongoing active-call notification should not ring.
            b.setSound(null);
            b.setVibrate(null);
        }
        NotificationManager nm = (NotificationManager) ctx.getSystemService(Context.NOTIFICATION_SERVICE);
        if (nm != null) {
            nm.cancel(NOTIF_INCOMING);
            nm.notify(NOTIF_ACTIVE, b.build());
        }
    }

    public static void cancelAll(Context ctx) {
        releaseWakeLock();
        NotificationManager nm = (NotificationManager) ctx.getSystemService(Context.NOTIFICATION_SERVICE);
        if (nm == null) return;
        nm.cancel(NOTIF_INCOMING);
        nm.cancel(NOTIF_ACTIVE);
    }
}
