package org.nyx.app;

import android.content.Context;
import android.graphics.ImageFormat;
import android.graphics.Rect;
import android.graphics.YuvImage;
import android.hardware.camera2.CameraAccessException;
import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CaptureRequest;
import android.media.Image;
import android.media.ImageReader;
import android.media.MediaRecorder;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;
import android.util.Size;
import android.view.Surface;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.nio.ByteBuffer;
import java.util.Arrays;
import java.util.Collections;

/**
 * Camera2 capture for Nyx calls — NO SurfaceView in the Activity hierarchy.
 * Qt Multimedia preview SurfaceView was stealing touches / ANRing the UI thread.
 * All HAL work runs on a dedicated HandlerThread.
 */
public final class NyxCameraCapture {
    private static final String TAG = "NyxCameraCapture";
    private static final int TARGET_W = 960;
    private static final int TARGET_H = 540;
    private static final long MIN_FRAME_INTERVAL_MS = 80; // ~12 fps capture; encode adapts

    private static final Object LOCK = new Object();
    private static HandlerThread sThread;
    private static Handler sHandler;
    private static CameraDevice sCamera;
    private static CameraCaptureSession sSession;
    private static ImageReader sReader;
    private static MediaRecorder sRecorder;
    private static Surface sRecorderSurface;
    private static Size sCaptureSize;
    private static Size sRecordSize;
    private static int sSensorOrientation = 90;
    private static String sRecordingPath = "";
    private static boolean sRecording = false;
    private static String sCameraId = "";
    private static boolean sFront = true;
    private static boolean sOpening = false;
    private static long sLastFrameMs = 0;
    private static Context sAppCtx;
    private static int sGeneration = 0;

    private NyxCameraCapture() {}

    public static void start(Context ctx, boolean preferFront) {
        if (ctx == null) return;
        final Context app = ctx.getApplicationContext();
        ensureThread();
        final int generation;
        synchronized (LOCK) {
            generation = ++sGeneration;
        }
        sHandler.post(() -> openLocked(app, preferFront, generation));
    }

    public static void stop() {
        ensureThread();
        synchronized (LOCK) {
            ++sGeneration;
        }
        sHandler.post(NyxCameraCapture::closeLocked);
    }

    public static void switchFacing() {
        ensureThread();
        sHandler.post(() -> {
            final boolean next = !sFront;
            final Context ctx = sAppCtx;
            closeLocked();
            final int generation;
            synchronized (LOCK) {
                generation = ++sGeneration;
            }
            if (ctx != null) openLocked(ctx, next, generation);
        });
    }

    public static void startRecording(String path) {
        if (path == null || path.isEmpty()) {
            nativeOnRecordingError("empty output path");
            return;
        }
        ensureThread();
        sHandler.post(() -> startRecordingLocked(path));
    }

    public static void stopRecording() {
        ensureThread();
        sHandler.post(NyxCameraCapture::stopRecordingLocked);
    }

    public static boolean hasFrontAndBack(Context ctx) {
        try {
            CameraManager cm = (CameraManager) ctx.getSystemService(Context.CAMERA_SERVICE);
            if (cm == null) return false;
            boolean front = false, back = false;
            for (String id : cm.getCameraIdList()) {
                CameraCharacteristics ch = cm.getCameraCharacteristics(id);
                Integer facing = ch.get(CameraCharacteristics.LENS_FACING);
                if (facing == null) continue;
                if (facing == CameraCharacteristics.LENS_FACING_FRONT) front = true;
                if (facing == CameraCharacteristics.LENS_FACING_BACK) back = true;
            }
            return front && back;
        } catch (Throwable t) {
            Log.w(TAG, "hasFrontAndBack", t);
            return false;
        }
    }

    private static void ensureThread() {
        synchronized (LOCK) {
            if (sThread != null) return;
            sThread = new HandlerThread("nyx-camera2");
            sThread.start();
            sHandler = new Handler(sThread.getLooper());
        }
    }

    private static void openLocked(Context app, boolean preferFront, int generation) {
        synchronized (LOCK) {
            if (generation != sGeneration) return;
        }
        closeLocked();
        sAppCtx = app;
        sFront = preferFront;
        sOpening = true;
        sLastFrameMs = 0;
        try {
            CameraManager cm = (CameraManager) app.getSystemService(Context.CAMERA_SERVICE);
            if (cm == null) throw new IllegalStateException("no CameraManager");
            final String id = pickCameraId(cm, preferFront);
            if (id == null) throw new IllegalStateException("no camera id");
            sCameraId = id;
            sCaptureSize = pickSize(cm, id);
            sRecordSize = pickRecordSize(cm, id);
            CameraCharacteristics characteristics = cm.getCameraCharacteristics(id);
            Integer orientation = characteristics.get(CameraCharacteristics.SENSOR_ORIENTATION);
            sSensorOrientation = orientation != null ? orientation : 90;
            Log.i(TAG, "open id=" + id + " front=" + preferFront + " preview="
                    + sCaptureSize.getWidth() + "x" + sCaptureSize.getHeight() + " record="
                    + sRecordSize.getWidth() + "x" + sRecordSize.getHeight()
                    + " sensor=" + sSensorOrientation);

            sReader = ImageReader.newInstance(sCaptureSize.getWidth(), sCaptureSize.getHeight(),
                    ImageFormat.YUV_420_888, 2);
            sReader.setOnImageAvailableListener(reader -> onImage(reader), sHandler);

            cm.openCamera(id, new CameraDevice.StateCallback() {
                @Override
                public void onOpened(CameraDevice camera) {
                    synchronized (LOCK) {
                        if (generation != sGeneration) {
                            camera.close();
                            return;
                        }
                    }
                    sCamera = camera;
                    sOpening = false;
                    try {
                        createPreviewSession(camera);
                        nativeOnStarted(sFront, sCameraId);
                    } catch (Throwable t) {
                        Log.e(TAG, "createSession failed", t);
                        nativeOnError("session: " + t.getMessage());
                        closeLocked();
                    }
                }

                @Override
                public void onDisconnected(CameraDevice camera) {
                    camera.close();
                    Log.w(TAG, "disconnected");
                    closeLocked();
                    nativeOnError("disconnected");
                }

                @Override
                public void onError(CameraDevice camera, int error) {
                    camera.close();
                    Log.e(TAG, "camera error " + error);
                    closeLocked();
                    nativeOnError("camera error " + error);
                }
            }, sHandler);
        } catch (SecurityException se) {
            sOpening = false;
            Log.e(TAG, "permission", se);
            nativeOnError("camera permission");
            closeLocked();
        } catch (Throwable t) {
            sOpening = false;
            Log.e(TAG, "open failed", t);
            nativeOnError("open: " + t.getMessage());
            closeLocked();
        }
    }

    private static void createPreviewSession(CameraDevice camera) throws CameraAccessException {
        final Surface surface = sReader.getSurface();
        camera.createCaptureSession(Collections.singletonList(surface),
                new CameraCaptureSession.StateCallback() {
                    @Override
                    public void onConfigured(CameraCaptureSession session) {
                        sSession = session;
                        try {
                            CaptureRequest.Builder b =
                                    camera.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW);
                            b.addTarget(surface);
                            b.set(CaptureRequest.CONTROL_MODE, CaptureRequest.CONTROL_MODE_AUTO);
                            b.set(CaptureRequest.CONTROL_AF_MODE,
                                    CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_PICTURE);
                            session.setRepeatingRequest(b.build(), null, sHandler);
                            Log.i(TAG, "repeating preview request started");
                        } catch (Throwable t) {
                            Log.e(TAG, "setRepeatingRequest", t);
                            nativeOnError("repeat: " + t.getMessage());
                            closeLocked();
                        }
                    }

                    @Override
                    public void onConfigureFailed(CameraCaptureSession session) {
                        Log.e(TAG, "configure failed");
                        nativeOnError("configure failed");
                        closeLocked();
                    }
                },
                sHandler);
    }

    private static void startRecordingLocked(String path) {
        if (sCamera == null || sReader == null || sRecording) {
            nativeOnRecordingError("camera is not ready");
            return;
        }
        try {
            File out = new File(path);
            File parent = out.getParentFile();
            if (parent != null && !parent.exists() && !parent.mkdirs())
                throw new IllegalStateException("cannot create output directory");
            if (out.exists() && !out.delete())
                throw new IllegalStateException("cannot replace output file");

            sRecorder = new MediaRecorder();
            sRecorder.setAudioSource(MediaRecorder.AudioSource.CAMCORDER);
            sRecorder.setVideoSource(MediaRecorder.VideoSource.SURFACE);
            sRecorder.setOutputFormat(MediaRecorder.OutputFormat.MPEG_4);
            sRecorder.setVideoEncoder(MediaRecorder.VideoEncoder.H264);
            sRecorder.setAudioEncoder(MediaRecorder.AudioEncoder.AAC);
            sRecorder.setVideoSize(sRecordSize.getWidth(), sRecordSize.getHeight());
            sRecorder.setVideoFrameRate(30);
            sRecorder.setVideoEncodingBitRate(4_000_000);
            sRecorder.setAudioSamplingRate(48_000);
            sRecorder.setAudioEncodingBitRate(128_000);
            sRecorder.setOrientationHint(recordingOrientation());
            sRecorder.setOutputFile(path);
            sRecorder.prepare();
            sRecorderSurface = sRecorder.getSurface();
            sRecordingPath = path;

            closeSessionOnly();
            sHandler.postDelayed(NyxCameraCapture::createRecordingSessionLocked, 120);
        } catch (Throwable t) {
            Log.e(TAG, "startRecording", t);
            releaseRecorder();
            nativeOnRecordingError("record: " + t.getMessage());
        }
    }

    private static void createRecordingSessionLocked() {
        if (sCamera == null || sReader == null || sRecorder == null
                || sRecorderSurface == null) {
            nativeOnRecordingError("recording session is not ready");
            return;
        }
        final Surface previewSurface = sReader.getSurface();
        try {
            sCamera.createCaptureSession(Arrays.asList(previewSurface, sRecorderSurface),
                    new CameraCaptureSession.StateCallback() {
                        @Override
                        public void onConfigured(CameraCaptureSession session) {
                            if (sCamera == null || sRecorder == null) {
                                session.close();
                                return;
                            }
                            sSession = session;
                            try {
                                CaptureRequest.Builder b = sCamera.createCaptureRequest(
                                        CameraDevice.TEMPLATE_RECORD);
                                b.addTarget(previewSurface);
                                b.addTarget(sRecorderSurface);
                                b.set(CaptureRequest.CONTROL_MODE,
                                        CaptureRequest.CONTROL_MODE_AUTO);
                                b.set(CaptureRequest.CONTROL_AF_MODE,
                                        CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO);
                                session.setRepeatingRequest(b.build(), null, sHandler);
                                sRecorder.start();
                                sRecording = true;
                                nativeOnRecordingStarted(sRecordingPath);
                                Log.i(TAG, "recording started " + sRecordingPath);
                            } catch (Throwable t) {
                                Log.e(TAG, "start recorder", t);
                                closeSessionOnly();
                                releaseRecorder();
                                nativeOnRecordingError("start: " + t.getMessage());
                            }
                        }

                        @Override
                        public void onConfigureFailed(CameraCaptureSession session) {
                            Log.e(TAG, "recording configure failed");
                            releaseRecorder();
                            nativeOnRecordingError("recording configure failed");
                            restorePreviewLater();
                        }
                    }, sHandler);
        } catch (Throwable t) {
            Log.e(TAG, "create recording session", t);
            releaseRecorder();
            nativeOnRecordingError("recording session: " + t.getMessage());
            restorePreviewLater();
        }
    }

    private static void stopRecordingLocked() {
        final String path = sRecordingPath;
        boolean ok = sRecording;
        if (sRecording && sRecorder != null) {
            try {
                sRecorder.stop();
            } catch (Throwable t) {
                ok = false;
                Log.e(TAG, "stop recorder", t);
            }
        }
        sRecording = false;
        closeSessionOnly();
        releaseRecorder();
        restorePreviewLater();
        nativeOnRecordingStopped(path, ok);
    }

    private static void restorePreviewLater() {
        sHandler.postDelayed(() -> {
            if (sCamera == null || sReader == null || sRecording) return;
            try {
                createPreviewSession(sCamera);
            } catch (Throwable t) {
                Log.e(TAG, "restore preview", t);
                nativeOnError("preview: " + t.getMessage());
            }
        }, 120);
    }

    private static void closeSessionOnly() {
        if (sSession == null) return;
        try {
            sSession.stopRepeating();
        } catch (Throwable ignored) {}
        try {
            sSession.close();
        } catch (Throwable ignored) {}
        sSession = null;
    }

    private static void releaseRecorder() {
        if (sRecorder != null) {
            try {
                sRecorder.reset();
            } catch (Throwable ignored) {}
            try {
                sRecorder.release();
            } catch (Throwable ignored) {}
        }
        sRecorder = null;
        sRecorderSurface = null;
        sRecording = false;
        sRecordingPath = "";
    }

    private static int recordingOrientation() {
        int degrees = 0;
        try {
            android.view.WindowManager wm = (android.view.WindowManager)
                    sAppCtx.getSystemService(Context.WINDOW_SERVICE);
            int rotation = wm != null ? wm.getDefaultDisplay().getRotation()
                                      : Surface.ROTATION_0;
            if (rotation == Surface.ROTATION_90) degrees = 90;
            else if (rotation == Surface.ROTATION_180) degrees = 180;
            else if (rotation == Surface.ROTATION_270) degrees = 270;
        } catch (Throwable ignored) {}
        if (sFront) {
            int result = (sSensorOrientation + degrees) % 360;
            return (360 - result) % 360;
        }
        return (sSensorOrientation - degrees + 360) % 360;
    }

    private static void onImage(ImageReader reader) {
        Image image = null;
        try {
            image = reader.acquireLatestImage();
            if (image == null) return;
            final long now = System.currentTimeMillis();
            if (now - sLastFrameMs < MIN_FRAME_INTERVAL_MS) return;
            sLastFrameMs = now;

            final int w = image.getWidth();
            final int h = image.getHeight();
            byte[] nv21 = yuv420ToNv21(image);
            if (nv21 == null) return;
            YuvImage yuv = new YuvImage(nv21, ImageFormat.NV21, w, h, null);
            ByteArrayOutputStream bos = new ByteArrayOutputStream(w * h / 4);
            // High-quality camera bridge; network compression is AV1 in C++.
            if (!yuv.compressToJpeg(new Rect(0, 0, w, h), 90, bos)) return;
            byte[] jpeg = bos.toByteArray();
            nativeOnJpeg(jpeg, w, h, sFront);
        } catch (Throwable t) {
            Log.w(TAG, "onImage", t);
        } finally {
            if (image != null) {
                try {
                    image.close();
                } catch (Throwable ignored) {}
            }
        }
    }

    private static byte[] yuv420ToNv21(Image image) {
        Image.Plane[] planes = image.getPlanes();
        if (planes == null || planes.length < 3) return null;
        final int width = image.getWidth();
        final int height = image.getHeight();
        final int ySize = width * height;
        final int uvSize = width * height / 2;
        byte[] out = new byte[ySize + uvSize];

        ByteBuffer yBuf = planes[0].getBuffer();
        ByteBuffer uBuf = planes[1].getBuffer();
        ByteBuffer vBuf = planes[2].getBuffer();
        final int yRow = planes[0].getRowStride();
        final int yPix = planes[0].getPixelStride();
        final int uRow = planes[1].getRowStride();
        final int uPix = planes[1].getPixelStride();
        final int vRow = planes[2].getRowStride();
        final int vPix = planes[2].getPixelStride();

        int dst = 0;
        if (yPix == 1 && yRow == width) {
            yBuf.get(out, 0, ySize);
            dst = ySize;
        } else {
            for (int row = 0; row < height; ++row) {
                int pos = row * yRow;
                for (int col = 0; col < width; ++col) {
                    out[dst++] = yBuf.get(pos + col * yPix);
                }
            }
        }

        // NV21 = YYYY + VUVU…
        final int uvHeight = height / 2;
        final int uvWidth = width / 2;
        for (int row = 0; row < uvHeight; ++row) {
            for (int col = 0; col < uvWidth; ++col) {
                final int uIndex = row * uRow + col * uPix;
                final int vIndex = row * vRow + col * vPix;
                out[dst++] = vBuf.get(vIndex);
                out[dst++] = uBuf.get(uIndex);
            }
        }
        return out;
    }

    private static String pickCameraId(CameraManager cm, boolean preferFront)
            throws CameraAccessException {
        final int want = preferFront ? CameraCharacteristics.LENS_FACING_FRONT
                                     : CameraCharacteristics.LENS_FACING_BACK;
        String fallback = null;
        for (String id : cm.getCameraIdList()) {
            CameraCharacteristics ch = cm.getCameraCharacteristics(id);
            Integer facing = ch.get(CameraCharacteristics.LENS_FACING);
            if (facing == null) continue;
            if (facing == want) return id;
            if (fallback == null) fallback = id;
        }
        return fallback;
    }

    private static Size pickSize(CameraManager cm, String id) throws CameraAccessException {
        CameraCharacteristics ch = cm.getCameraCharacteristics(id);
        android.hardware.camera2.params.StreamConfigurationMap map =
                ch.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
        Size best = new Size(TARGET_W, TARGET_H);
        if (map == null) return best;
        Size[] sizes = map.getOutputSizes(ImageFormat.YUV_420_888);
        if (sizes == null || sizes.length == 0) return best;
        long bestScore = Long.MAX_VALUE;
        for (Size s : sizes) {
            final long area = (long) s.getWidth() * s.getHeight();
            final long target = (long) TARGET_W * TARGET_H;
            // Prefer near target, never huge (CPU JPEG cost).
            if (area > target * 4) continue;
            long score = Math.abs(area - target);
            if (score < bestScore) {
                bestScore = score;
                best = s;
            }
        }
        return best;
    }

    private static Size pickRecordSize(CameraManager cm, String id)
            throws CameraAccessException {
        CameraCharacteristics ch = cm.getCameraCharacteristics(id);
        android.hardware.camera2.params.StreamConfigurationMap map =
                ch.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
        Size fallback = new Size(1280, 720);
        if (map == null) return fallback;
        Size[] sizes = map.getOutputSizes(MediaRecorder.class);
        if (sizes == null || sizes.length == 0) return fallback;
        Size best = sizes[0];
        long bestScore = Long.MAX_VALUE;
        final long targetArea = 1280L * 720L;
        for (Size size : sizes) {
            long area = (long) size.getWidth() * size.getHeight();
            if (area > 1920L * 1080L) continue;
            long aspectPenalty = Math.abs(size.getWidth() * 9L - size.getHeight() * 16L)
                    * 1000L;
            long score = Math.abs(area - targetArea) + aspectPenalty;
            if (score < bestScore) {
                bestScore = score;
                best = size;
            }
        }
        return best;
    }

    private static void closeLocked() {
        sOpening = false;
        if (sRecording && sRecorder != null) {
            try {
                sRecorder.stop();
            } catch (Throwable ignored) {}
        }
        try {
            closeSessionOnly();
        } catch (Throwable ignored) {}
        releaseRecorder();
        try {
            if (sCamera != null) {
                sCamera.close();
                sCamera = null;
            }
        } catch (Throwable ignored) {}
        try {
            if (sReader != null) {
                sReader.close();
                sReader = null;
            }
        } catch (Throwable ignored) {}
        Log.i(TAG, "closed");
    }

    private static native void nativeOnJpeg(byte[] jpeg, int width, int height, boolean front);
    private static native void nativeOnError(String message);
    private static native void nativeOnStarted(boolean front, String cameraId);
    private static native void nativeOnRecordingStarted(String path);
    private static native void nativeOnRecordingStopped(String path, boolean success);
    private static native void nativeOnRecordingError(String message);
}
