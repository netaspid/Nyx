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
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;
import android.util.Size;
import android.view.Surface;

import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;
import java.util.Collections;

/**
 * Camera2 capture for Nyx calls — NO SurfaceView in the Activity hierarchy.
 * Qt Multimedia preview SurfaceView was stealing touches / ANRing the UI thread.
 * All HAL work runs on a dedicated HandlerThread.
 */
public final class NyxCameraCapture {
    private static final String TAG = "NyxCameraCapture";
    private static final int TARGET_W = 640;
    private static final int TARGET_H = 360;
    private static final long MIN_FRAME_INTERVAL_MS = 330; // ~3 fps

    private static final Object LOCK = new Object();
    private static HandlerThread sThread;
    private static Handler sHandler;
    private static CameraDevice sCamera;
    private static CameraCaptureSession sSession;
    private static ImageReader sReader;
    private static String sCameraId = "";
    private static boolean sFront = true;
    private static boolean sOpening = false;
    private static long sLastFrameMs = 0;
    private static Context sAppCtx;

    private NyxCameraCapture() {}

    public static void start(Context ctx, boolean preferFront) {
        if (ctx == null) return;
        final Context app = ctx.getApplicationContext();
        ensureThread();
        sHandler.post(() -> openLocked(app, preferFront));
    }

    public static void stop() {
        ensureThread();
        sHandler.post(NyxCameraCapture::closeLocked);
    }

    public static void switchFacing() {
        ensureThread();
        sHandler.post(() -> {
            final boolean next = !sFront;
            final Context ctx = sAppCtx;
            closeLocked();
            if (ctx != null) openLocked(ctx, next);
        });
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

    private static void openLocked(Context app, boolean preferFront) {
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
            Size sz = pickSize(cm, id);
            Log.i(TAG, "open id=" + id + " front=" + preferFront + " size=" + sz.getWidth() + "x"
                    + sz.getHeight());

            sReader = ImageReader.newInstance(sz.getWidth(), sz.getHeight(),
                    ImageFormat.YUV_420_888, 2);
            sReader.setOnImageAvailableListener(reader -> onImage(reader), sHandler);

            cm.openCamera(id, new CameraDevice.StateCallback() {
                @Override
                public void onOpened(CameraDevice camera) {
                    sCamera = camera;
                    sOpening = false;
                    try {
                        createSession(camera);
                        nativeOnStarted(sFront, sCameraId);
                    } catch (Throwable t) {
                        Log.e(TAG, "createSession failed", t);
                        nativeOnError("session: " + t.getMessage());
                        closeLocked();
                    }
                }

                @Override
                public void onDisconnected(CameraDevice camera) {
                    Log.w(TAG, "disconnected");
                    closeLocked();
                    nativeOnError("disconnected");
                }

                @Override
                public void onError(CameraDevice camera, int error) {
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

    private static void createSession(CameraDevice camera) throws CameraAccessException {
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
            // Scale via JPEG quality; Qt will cover-crop to encode size.
            if (!yuv.compressToJpeg(new Rect(0, 0, w, h), 55, bos)) return;
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

    private static void closeLocked() {
        sOpening = false;
        try {
            if (sSession != null) {
                try {
                    sSession.stopRepeating();
                } catch (Throwable ignored) {}
                try {
                    sSession.close();
                } catch (Throwable ignored) {}
                sSession = null;
            }
        } catch (Throwable ignored) {}
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
}
