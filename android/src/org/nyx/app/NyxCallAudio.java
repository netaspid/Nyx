package org.nyx.app;

import android.content.Context;
import android.media.AudioAttributes;
import android.media.AudioDeviceInfo;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioRecord;
import android.media.AudioTrack;
import android.media.MediaRecorder;
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

public final class NyxCallAudio {
    private static final String TAG = "NyxCallAudio";
    private static final Handler MAIN = new Handler(Looper.getMainLooper());
    private static AudioTrack sTrack;
    private static AudioRecord sRecord;
    private static Ringtone sRing;
    private static Vibrator sVibrator;
    private static int sSampleRate = 48000;
    private static int sChannels = 1;
    private static boolean sSpeaker = true;
    private static int sWriteLogCounter = 0;
    private static long sWriteBytes = 0;
    private static int sReadLogCounter = 0;
    private static long sReadBytes = 0;

    private NyxCallAudio() {}

    public static synchronized void startVoicePlayback(int sampleRate, int channels) {
        startVoicePlayback(sampleRate, channels, sSpeaker);
    }

    public static synchronized void startVoicePlayback(int sampleRate, int channels,
                                                       boolean speaker) {
        stopVoicePlayback();
        if (sampleRate <= 0) sampleRate = 48000;
        if (channels <= 0) channels = 1;
        sSampleRate = sampleRate;
        sChannels = channels;
        sSpeaker = speaker;
        sWriteLogCounter = 0;
        sWriteBytes = 0;
        final int chMask = channels >= 2 ? AudioFormat.CHANNEL_OUT_STEREO
                                         : AudioFormat.CHANNEL_OUT_MONO;
        int minBuf = AudioTrack.getMinBufferSize(sampleRate, chMask, AudioFormat.ENCODING_PCM_16BIT);
        if (minBuf <= 0) minBuf = sampleRate * channels * 2 / 5;

        final int buf = Math.max(minBuf * 4, sampleRate * channels * 2 / 5);
        try {

            final int usage = speaker ? AudioAttributes.USAGE_MEDIA
                                      : AudioAttributes.USAGE_VOICE_COMMUNICATION;
            final int content = speaker ? AudioAttributes.CONTENT_TYPE_MUSIC
                                        : AudioAttributes.CONTENT_TYPE_SPEECH;
            AudioAttributes aa = new AudioAttributes.Builder()
                    .setUsage(usage)
                    .setContentType(content)
                    .build();
            AudioFormat fmt = new AudioFormat.Builder()
                    .setSampleRate(sampleRate)
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .setChannelMask(chMask)
                    .build();
            if (Build.VERSION.SDK_INT >= 23) {
                AudioTrack.Builder tb = new AudioTrack.Builder()
                        .setAudioAttributes(aa)
                        .setAudioFormat(fmt)
                        .setBufferSizeInBytes(buf)
                        .setTransferMode(AudioTrack.MODE_STREAM);
                sTrack = tb.build();
            } else {
                sTrack = new AudioTrack(aa, fmt, buf, AudioTrack.MODE_STREAM,
                        AudioManager.AUDIO_SESSION_ID_GENERATE);
            }
            try {
                sTrack.setVolume(1.0f);
            } catch (Throwable ignored) {}
            sTrack.play();

            byte[] silence = new byte[Math.min(buf / 4, sampleRate * channels * 2 / 25)];
            sTrack.write(silence, 0, silence.length);
            Log.i(TAG, "voice playback started sr=" + sampleRate + " ch=" + channels
                    + " buf=" + buf + " speaker=" + speaker + " usage="
                    + (speaker ? "MEDIA" : "VOICE"));
        } catch (Throwable t) {
            Log.e(TAG, "startVoicePlayback failed", t);
            sTrack = null;
        }
    }


    public static synchronized void restartVoicePlaybackForRoute(Context ctx, boolean speaker) {
        if (sTrack == null) {
            sSpeaker = speaker;
            return;
        }
        if (sSpeaker == speaker) {
            applyPlaybackRoute(ctx, speaker);
            return;
        }
        final int sr = sSampleRate;
        final int ch = sChannels;
        startVoicePlayback(sr, ch, speaker);
        applyPlaybackRoute(ctx, speaker);
    }

    public static synchronized int writeVoicePlayback(byte[] pcm, int offset, int len) {
        if (sTrack == null || pcm == null || len <= 0) return 0;
        try {

            final int end = Math.min(offset + len, pcm.length);
            for (int i = offset; i + 1 < end; i += 2) {
                short s = (short) ((pcm[i] & 0xff) | ((pcm[i + 1] & 0xff) << 8));
                int v = (s * 8) / 5;
                if (v > 32767) v = 32767;
                if (v < -32768) v = -32768;
                pcm[i] = (byte) (v & 0xff);
                pcm[i + 1] = (byte) ((v >> 8) & 0xff);
            }
            int written = sTrack.write(pcm, offset, len);
            sWriteBytes += Math.max(0, written);
            if ((++sWriteLogCounter % 50) == 0) {
                Log.i(TAG, "write ok n=" + written + " totalBytes=" + sWriteBytes
                        + " state=" + sTrack.getPlayState());
            }
            return written;
        } catch (Throwable t) {
            Log.e(TAG, "writeVoicePlayback failed", t);
            return 0;
        }
    }


    public static synchronized void applyPlaybackRoute(Context ctx, boolean speaker) {
        if (sTrack == null || ctx == null) return;
        try {
            if (Build.VERSION.SDK_INT < 23) return;
            AudioManager am = (AudioManager) ctx.getSystemService(Context.AUDIO_SERVICE);
            if (am == null) return;
            AudioDeviceInfo[] outs = am.getDevices(AudioManager.GET_DEVICES_OUTPUTS);
            if (outs == null) return;
            final int want = speaker ? AudioDeviceInfo.TYPE_BUILTIN_SPEAKER
                                     : AudioDeviceInfo.TYPE_BUILTIN_EARPIECE;
            for (AudioDeviceInfo d : outs) {
                if (d.getType() == want) {
                    boolean ok = sTrack.setPreferredDevice(d);
                    Log.i(TAG, "AudioTrack preferredDevice type=" + want + " ok=" + ok);
                    return;
                }
            }
        } catch (Throwable t) {
            Log.w(TAG, "applyPlaybackRoute failed", t);
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


    public static synchronized boolean startVoiceCapture(int sampleRate, int channels) {
        stopVoiceCapture();
        if (sampleRate <= 0) sampleRate = 48000;
        if (channels <= 0) channels = 1;
        final int chMask = channels >= 2 ? AudioFormat.CHANNEL_IN_STEREO
                                         : AudioFormat.CHANNEL_IN_MONO;
        int minBuf = AudioRecord.getMinBufferSize(sampleRate, chMask, AudioFormat.ENCODING_PCM_16BIT);
        if (minBuf <= 0) {
            Log.e(TAG, "getMinBufferSize failed for capture");
            return false;
        }
        final int buf = Math.max(minBuf * 4, sampleRate * channels * 2 / 5);
        final int[] sources = new int[] {
                MediaRecorder.AudioSource.VOICE_COMMUNICATION,
                MediaRecorder.AudioSource.MIC,
                MediaRecorder.AudioSource.DEFAULT
        };
        for (int src : sources) {
            try {
                AudioRecord rec = new AudioRecord(src, sampleRate, chMask,
                        AudioFormat.ENCODING_PCM_16BIT, buf);
                if (rec.getState() != AudioRecord.STATE_INITIALIZED) {
                    rec.release();
                    continue;
                }
                rec.startRecording();
                sRecord = rec;
                sSampleRate = sampleRate;
                sChannels = channels;
                sReadLogCounter = 0;
                sReadBytes = 0;
                Log.i(TAG, "voice capture started sr=" + sampleRate + " ch=" + channels
                        + " src=" + src + " buf=" + buf);
                return true;
            } catch (Throwable t) {
                Log.w(TAG, "AudioRecord src=" + src + " failed", t);
            }
        }
        Log.e(TAG, "startVoiceCapture failed for all sources");
        sRecord = null;
        return false;
    }

    public static synchronized int readVoiceCapture(byte[] pcm, int offset, int len) {
        if (sRecord == null || pcm == null || len <= 0) return 0;
        try {
            int n;
            if (Build.VERSION.SDK_INT >= 23) {
                n = sRecord.read(pcm, offset, len, AudioRecord.READ_NON_BLOCKING);
            } else {
                n = sRecord.read(pcm, offset, len);
            }
            if (n > 0) {
                sReadBytes += n;
                if ((++sReadLogCounter % 50) == 0) {

                    int lim = Math.min(n, 128);
                    long sum = 0;
                    for (int i = offset; i + 1 < offset + lim; i += 2) {
                        short s = (short) ((pcm[i] & 0xff) | ((pcm[i + 1] & 0xff) << 8));
                        sum += (long) s * s;
                    }
                    double rms = Math.sqrt(sum / Math.max(1.0, lim / 2.0)) / 32768.0;
                    Log.i(TAG, "capture read n=" + n + " totalBytes=" + sReadBytes
                            + " rms=" + String.format("%.3f", rms));
                }
            }
            return Math.max(0, n);
        } catch (Throwable t) {
            Log.e(TAG, "readVoiceCapture failed", t);
            return 0;
        }
    }

    public static synchronized void stopVoiceCapture() {
        if (sRecord == null) return;
        try {
            sRecord.stop();
        } catch (Throwable ignored) {}
        try {
            sRecord.release();
        } catch (Throwable ignored) {}
        sRecord = null;
        Log.i(TAG, "voice capture stopped");
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


    public static synchronized void playTestTone(Context ctx, int sampleRate, int durationMs) {
        if (ctx == null) return;
        if (sampleRate <= 0) sampleRate = 48000;
        if (durationMs <= 0) durationMs = 700;
        boostCallVolumes(ctx);
        final int sr = sampleRate;
        final int dur = durationMs;
        new Thread(() -> {
            AudioTrack track = null;
            try {
                final int chMask = AudioFormat.CHANNEL_OUT_MONO;
                int minBuf = AudioTrack.getMinBufferSize(sr, chMask, AudioFormat.ENCODING_PCM_16BIT);
                if (minBuf <= 0) minBuf = sr * 2 / 5;
                AudioAttributes aa = new AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_MEDIA)
                        .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                        .build();
                AudioFormat fmt = new AudioFormat.Builder()
                        .setSampleRate(sr)
                        .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                        .setChannelMask(chMask)
                        .build();
                track = new AudioTrack.Builder()
                        .setAudioAttributes(aa)
                        .setAudioFormat(fmt)
                        .setBufferSizeInBytes(Math.max(minBuf * 2, sr))
                        .setTransferMode(AudioTrack.MODE_STREAM)
                        .build();
                track.play();
                final int n = sr * dur / 1000;
                byte[] pcm = new byte[n * 2];
                for (int i = 0; i < n; ++i) {
                    double t = (double) i / (double) sr;
                    short s = (short) (Math.sin(2.0 * Math.PI * 440.0 * t) * 12000.0);
                    pcm[i * 2] = (byte) (s & 0xff);
                    pcm[i * 2 + 1] = (byte) ((s >> 8) & 0xff);
                }
                track.write(pcm, 0, pcm.length);
                try { Thread.sleep(dur + 50); } catch (InterruptedException ignored) {}
            } catch (Throwable t) {
                Log.e(TAG, "playTestTone failed", t);
            } finally {
                if (track != null) {
                    try { track.stop(); } catch (Throwable ignored) {}
                    try { track.release(); } catch (Throwable ignored) {}
                }
            }
        }, "nyx-tone").start();
    }
}
