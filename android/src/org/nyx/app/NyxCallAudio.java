package org.nyx.app;

import android.content.Context;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import android.media.Ringtone;
import android.media.RingtoneManager;
import android.net.Uri;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.os.VibratorManager;
import android.util.Log;

/**
 * VoIP playback + ringtone. Qt QAudioSink uses STREAM_MUSIC which is silent
 * under AudioManager.MODE_IN_COMMUNICATION — AudioTrack VOICE_COMMUNICATION works.
 * Ringtone must run on the main looper (JNI often arrives on Qt's thread).
 */
public final class NyxCallAudio {
    private static final String TAG = "NyxCallAudio";
    private static final Handler MAIN = new Handler(Looper.getMainLooper());
    private static AudioTrack sTrack;
    private static Ringtone sRing;
    private static Vibrator sVibrator;
    private static int sSampleRate = 48000;
    private static int sChannels = 1;

    private NyxCallAudio() {}

    public static synchronized void startVoicePlayback(int sampleRate, int channels) {
        stopVoicePlayback();
        if (sampleRate <= 0) sampleRate = 48000;
        if (channels <= 0) channels = 1;
        sSampleRate = sampleRate;
        sChannels = channels;
        final int chMask = channels >= 2 ? AudioFormat.CHANNEL_OUT_STEREO
                                         : AudioFormat.CHANNEL_OUT_MONO;
        int minBuf = AudioTrack.getMinBufferSize(sampleRate, chMask, AudioFormat.ENCODING_PCM_16BIT);
        if (minBuf <= 0) minBuf = sampleRate * channels * 2 / 5;
        final int buf = Math.max(minBuf * 2, sampleRate * channels * 2 / 5);
        try {
            AudioAttributes aa = new AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_VOICE_COMMUNICATION)
                    .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                    .build();
            AudioFormat fmt = new AudioFormat.Builder()
                    .setSampleRate(sampleRate)
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .setChannelMask(chMask)
                    .build();
            if (Build.VERSION.SDK_INT >= 23) {
                sTrack = new AudioTrack.Builder()
                        .setAudioAttributes(aa)
                        .setAudioFormat(fmt)
                        .setBufferSizeInBytes(buf)
                        .setTransferMode(AudioTrack.MODE_STREAM)
                        .build();
            } else {
                sTrack = new AudioTrack(aa, fmt, buf, AudioTrack.MODE_STREAM,
                        AudioManager.AUDIO_SESSION_ID_GENERATE);
            }
            sTrack.play();
            Log.i(TAG, "voice playback started sr=" + sampleRate + " ch=" + channels
                    + " buf=" + buf);
        } catch (Throwable t) {
            Log.e(TAG, "startVoicePlayback failed", t);
            sTrack = null;
        }
    }

    public static synchronized int writeVoicePlayback(byte[] pcm, int offset, int len) {
        if (sTrack == null || pcm == null || len <= 0) return 0;
        try {
            return sTrack.write(pcm, offset, len);
        } catch (Throwable t) {
            Log.e(TAG, "writeVoicePlayback failed", t);
            return 0;
        }
    }

    public static synchronized void stopVoicePlayback() {
        if (sTrack == null) return;
        try {
            sTrack.pause();
            sTrack.flush();
            sTrack.stop();
        } catch (Throwable ignored) {}
        try {
            sTrack.release();
        } catch (Throwable ignored) {}
        sTrack = null;
        Log.i(TAG, "voice playback stopped");
    }

    public static void startRingtone(Context ctx) {
        if (ctx == null) return;
        final Context app = ctx.getApplicationContext();
        MAIN.post(() -> startRingtoneOnMain(app));
    }

    private static void startRingtoneOnMain(Context ctx) {
        stopRingtoneOnMain();
        try {
            AudioManager am = (AudioManager) ctx.getSystemService(Context.AUDIO_SERVICE);
            if (am != null) {
                // Leave MODE_NORMAL while ringing — IN_COMMUNICATION mutes ringtone streams.
                if (am.getMode() == AudioManager.MODE_IN_COMMUNICATION
                        || am.getMode() == AudioManager.MODE_IN_CALL) {
                    am.setMode(AudioManager.MODE_NORMAL);
                }
                int max = am.getStreamMaxVolume(AudioManager.STREAM_RING);
                if (max > 0) {
                    int want = Math.max(am.getStreamVolume(AudioManager.STREAM_RING), (max * 3) / 4);
                    am.setStreamVolume(AudioManager.STREAM_RING, Math.min(want, max), 0);
                }
            }

            Uri uri = RingtoneManager.getDefaultUri(RingtoneManager.TYPE_RINGTONE);
            if (uri == null) uri = RingtoneManager.getDefaultUri(RingtoneManager.TYPE_NOTIFICATION);
            Ringtone ring = RingtoneManager.getRingtone(ctx, uri);
            if (ring == null) {
                Log.e(TAG, "getRingtone returned null");
                startVibrateOnMain(ctx);
                return;
            }
            if (Build.VERSION.SDK_INT >= 28) {
                ring.setLooping(true);
            }
            AudioAttributes aa = new AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_NOTIFICATION_RINGTONE)
                    .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                    .build();
            ring.setAudioAttributes(aa);
            ring.play();
            sRing = ring;
            startVibrateOnMain(ctx);
            Log.i(TAG, "ringtone started uri=" + uri);
        } catch (Throwable t) {
            Log.e(TAG, "startRingtone failed", t);
            sRing = null;
            startVibrateOnMain(ctx);
        }
    }

    private static void startVibrateOnMain(Context ctx) {
        try {
            Vibrator vib;
            if (Build.VERSION.SDK_INT >= 31) {
                VibratorManager vm = (VibratorManager) ctx.getSystemService(Context.VIBRATOR_MANAGER_SERVICE);
                vib = vm != null ? vm.getDefaultVibrator() : null;
            } else {
                vib = (Vibrator) ctx.getSystemService(Context.VIBRATOR_SERVICE);
            }
            if (vib == null || !vib.hasVibrator()) return;
            long[] pattern = new long[]{0, 500, 400, 500, 400, 500};
            if (Build.VERSION.SDK_INT >= 26) {
                vib.vibrate(VibrationEffect.createWaveform(pattern, 0));
            } else {
                vib.vibrate(pattern, 0);
            }
            sVibrator = vib;
        } catch (Throwable t) {
            Log.e(TAG, "vibrate failed", t);
        }
    }

    public static void stopRingtone() {
        MAIN.post(NyxCallAudio::stopRingtoneOnMain);
    }

    private static void stopRingtoneOnMain() {
        if (sRing != null) {
            try {
                if (sRing.isPlaying()) sRing.stop();
            } catch (Throwable ignored) {}
            sRing = null;
            Log.i(TAG, "ringtone stopped");
        }
        if (sVibrator != null) {
            try {
                sVibrator.cancel();
            } catch (Throwable ignored) {}
            sVibrator = null;
        }
    }

    /** Raise call + music volumes so both AudioTrack and any Qt path are audible. */
    public static void boostCallVolumes(Context ctx) {
        try {
            AudioManager am = (AudioManager) ctx.getSystemService(Context.AUDIO_SERVICE);
            if (am == null) return;
            int[] streams = new int[] {
                    AudioManager.STREAM_VOICE_CALL,
                    AudioManager.STREAM_MUSIC,
                    AudioManager.STREAM_RING
            };
            for (int stream : streams) {
                int max = am.getStreamMaxVolume(stream);
                if (max <= 0) continue;
                int want = Math.max(am.getStreamVolume(stream), (max * 3) / 4);
                am.setStreamVolume(stream, Math.min(want, max), 0);
            }
        } catch (Throwable t) {
            Log.e(TAG, "boostCallVolumes failed", t);
        }
    }
}
