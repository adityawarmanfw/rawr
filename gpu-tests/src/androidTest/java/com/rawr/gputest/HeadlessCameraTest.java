package com.rawr.gputest;

import android.Manifest;
import android.content.Context;
import android.content.Intent;
import android.content.ContentValues;
import android.content.pm.PackageManager;
import android.graphics.ImageFormat;
import android.hardware.camera2.*;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.media.Image;
import android.media.ImageReader;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.SystemClock;
import android.os.ParcelFileDescriptor;
import android.graphics.BitmapFactory;
import android.graphics.Bitmap;
import android.net.Uri;
import android.provider.MediaStore;
import android.util.Size;
import android.view.Surface;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.json.JSONObject;
import org.json.JSONArray;
import java.io.File;
import java.io.FileOutputStream;
import java.io.FileInputStream;
import java.nio.ByteBuffer;
import java.util.Arrays;
import java.util.Comparator;
import java.util.Collections;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import com.rawr.camera.renderer.RendererNative;
import static org.junit.Assert.*;

/** Explicitly opt-in real sensor tests. No Activity or display surface. */
@RunWith(AndroidJUnit4.class)
public final class HeadlessCameraTest {
    private static final int TIMEOUT_SECONDS = 20;
    private static <T> T take(ArrayBlockingQueue<T> queue, String label) throws Exception {
        T value = queue.poll(TIMEOUT_SECONDS, TimeUnit.SECONDS);
        assertNotNull(label + " timed out", value);
        return value;
    }
    @Test public void rawSensorCaptureDngAndCleanup() throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        assertEquals("Grant CAMERA with adb before this test", PackageManager.PERMISSION_GRANTED,
            context.checkSelfPermission(Manifest.permission.CAMERA));
        CameraManager manager = context.getSystemService(CameraManager.class);
        String requested = InstrumentationRegistry.getArguments().getString("cameraId");
        String cameraId = null;
        CameraCharacteristics characteristics = null;
        for (String id : manager.getCameraIdList()) {
            if (requested != null && !requested.equals(id)) continue;
            CameraCharacteristics candidate = manager.getCameraCharacteristics(id);
            int[] caps = candidate.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES);
            if (caps != null && Arrays.stream(caps).anyMatch(v -> v == CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_RAW)) {
                cameraId = id; characteristics = candidate; break;
            }
        }
        assertNotNull("No RAW-capable requested camera", cameraId);
        StreamConfigurationMap streams = characteristics.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
        assertNotNull(streams);
        Size[] sizes = streams.getOutputSizes(ImageFormat.RAW_SENSOR);
        assertNotNull(sizes);
        Size size = Arrays.stream(sizes).max(Comparator.comparingLong(s -> (long)s.getWidth() * s.getHeight())).orElseThrow();
        int samples = Integer.parseInt(InstrumentationRegistry.getArguments().getString("samples", "3"));
        assertTrue(samples >= 1 && samples <= 100);
        File dir = new File(context.getFilesDir(), "gpu-results/camera-" + cameraId);
        assertTrue(dir.exists() || dir.mkdirs());
        HandlerThread thread = new HandlerThread("rawr-headless-camera");
        thread.start();
        Handler handler = new Handler(thread.getLooper());
        ImageReader reader = ImageReader.newInstance(size.getWidth(), size.getHeight(), ImageFormat.RAW_SENSOR, 2);
        ArrayBlockingQueue<Image> images = new ArrayBlockingQueue<>(2);
        reader.setOnImageAvailableListener(source -> {
            Image image = source.acquireNextImage();
            if (image != null && !images.offer(image)) image.close();
        }, handler);
        ArrayBlockingQueue<TotalCaptureResult> results = new ArrayBlockingQueue<>(2);
        AtomicReference<Throwable> failure = new AtomicReference<>();
        AtomicReference<CameraDevice> cameraRef = new AtomicReference<>();
        AtomicReference<CameraCaptureSession> sessionRef = new AtomicReference<>();
        CountDownLatch opened = new CountDownLatch(1), configured = new CountDownLatch(1);
        HeadlessCameraService.resetReady();
        Intent service = new Intent(context, HeadlessCameraService.class);
        try {
            context.startForegroundService(service);
            assertTrue("camera foreground service timed out", HeadlessCameraService.awaitReady());
            manager.openCamera(cameraId, new CameraDevice.StateCallback() {
                @Override public void onOpened(CameraDevice device) { cameraRef.set(device); opened.countDown(); }
                @Override public void onDisconnected(CameraDevice device) { failure.set(new IllegalStateException("camera disconnected")); opened.countDown(); device.close(); }
                @Override public void onError(CameraDevice device, int code) { failure.set(new IllegalStateException("camera error " + code)); opened.countDown(); device.close(); }
            }, handler);
            assertTrue(opened.await(TIMEOUT_SECONDS, TimeUnit.SECONDS));
            assertNull(failure.get());
            CameraDevice camera = cameraRef.get();
            assertNotNull(camera);
            camera.createCaptureSession(Collections.singletonList(reader.getSurface()), new CameraCaptureSession.StateCallback() {
                @Override public void onConfigured(CameraCaptureSession session) { sessionRef.set(session); configured.countDown(); }
                @Override public void onConfigureFailed(CameraCaptureSession session) { failure.set(new IllegalStateException("RAW session configure failed")); configured.countDown(); }
            }, handler);
            assertTrue(configured.await(TIMEOUT_SECONDS, TimeUnit.SECONDS));
            assertNull(failure.get());
            CameraCaptureSession session = sessionRef.get();
            assertNotNull(session);
            JSONArray rows = new JSONArray();
            for (int index = 0; index < samples; ++index) {
                CaptureRequest.Builder request = camera.createCaptureRequest(CameraDevice.TEMPLATE_STILL_CAPTURE);
                request.addTarget(reader.getSurface());
                request.set(CaptureRequest.CONTROL_MODE, CaptureRequest.CONTROL_MODE_AUTO);
                long begin = SystemClock.elapsedRealtimeNanos();
                session.capture(request.build(), new CameraCaptureSession.CaptureCallback() {
                    @Override public void onCaptureCompleted(CameraCaptureSession s, CaptureRequest r, TotalCaptureResult result) {
                        if (!results.offer(result)) failure.set(new IllegalStateException("result queue overflow"));
                    }
                    @Override public void onCaptureFailed(CameraCaptureSession s, CaptureRequest r, CaptureFailure error) {
                        failure.set(new IllegalStateException("capture failed reason=" + error.getReason()));
                    }
                }, handler);
                TotalCaptureResult result = take(results, "capture result");
                try (Image image = take(images, "RAW image")) {
                    long acquired = SystemClock.elapsedRealtimeNanos();
                    Long timestamp = result.get(CaptureResult.SENSOR_TIMESTAMP);
                    assertNotNull(timestamp);
                    assertEquals("RAW/result timestamp mismatch", timestamp.longValue(), image.getTimestamp());
                    assertEquals(ImageFormat.RAW_SENSOR, image.getFormat());
                    Image.Plane plane = image.getPlanes()[0];
                    assertTrue(plane.getRowStride() >= size.getWidth() * 2);
                    ByteBuffer pixels = plane.getBuffer();
                    assertTrue(pixels.remaining() >= (long)(size.getHeight() - 1) * plane.getRowStride() + size.getWidth() * 2);
                    File file = new File(dir, "capture-" + index + ".dng");
                    try (DngCreator creator = new DngCreator(characteristics, result); FileOutputStream out = new FileOutputStream(file)) {
                        creator.setDescription("RAWR headless Camera2 sensor fixture; no production still processing applied");
                        creator.setOrientation(1); // Explicit sensor-native TIFF orientation, not Android's unspecified 0.
                        creator.writeImage(out, image);
                        out.getFD().sync();
                    }
                    assertTrue(file.length() > (long)size.getWidth() * size.getHeight());
                    rows.put(new JSONObject().put("sample", index).put("sensorTimestampNs", timestamp)
                        .put("width", size.getWidth()).put("height", size.getHeight())
                        .put("rowStride", plane.getRowStride()).put("bytes", file.length())
                        .put("requestToAcquiredMs", (acquired - begin) / 1e6)
                        .put("requestToDurableDngMs", (SystemClock.elapsedRealtimeNanos() - begin) / 1e6));
                    try (FileOutputStream report = new FileOutputStream(new File(dir, "captures.json"))) {
                        report.write(rows.toString(2).getBytes(java.nio.charset.StandardCharsets.UTF_8));
                    }
                }
                assertNull(failure.get());
            }
        } finally {
            CameraCaptureSession session = sessionRef.get();
            if (session != null) session.close();
            CameraDevice camera = cameraRef.get();
            if (camera != null) camera.close();
            Image remaining;
            while ((remaining = images.poll()) != null) remaining.close();
            reader.close();
            context.stopService(service);
            thread.quitSafely();
            thread.join(5000);
        }
        // Sensor capture is closed before production Vulkan development. This
        // replays a real RAW through the app renderer/export code, not Camera2's
        // vendor JPEG. It does not yet reproduce the live still coordinator.
        if (Boolean.parseBoolean(InstrumentationRegistry.getArguments().getString("productionReplay", "true"))) {
            File source = new File(dir, "capture-0.dng");
            RendererNative renderer = new RendererNative();
            long id = renderer.open(source.getAbsolutePath(), context.getFilesDir().getAbsolutePath(),
                context.getApplicationInfo().nativeLibraryDir, context.getAssets());
            assertTrue(id != 0);
            try {
                String algorithm = InstrumentationRegistry.getArguments().getString("demosaic", "Rcd");
                String recipe = new JSONObject().put("demosaicAlgorithm", algorithm)
                    .put("highlightReconstructionEnabled", true).put("highlightReconstructionMethod", 1)
                    .put("fccSteps", 2).put("defringeEnabled", true)
                    .put("tone", new JSONObject().put("jpegQuality", 98).put("jpegChromaSubsamplingId", "jpeg.422"))
                    .toString();
                long begin = SystemClock.elapsedRealtimeNanos();
                File jpeg = new File(dir, "production-replay.jpg");
                try (ParcelFileDescriptor output = ParcelFileDescriptor.open(jpeg,
                        ParcelFileDescriptor.MODE_CREATE | ParcelFileDescriptor.MODE_TRUNCATE | ParcelFileDescriptor.MODE_READ_WRITE)) {
                    renderer.render(id, recipe, size.getWidth(), size.getHeight(), output.getFd(), null,
                        size.getWidth(), size.getHeight());
                }
                BitmapFactory.Options bounds = new BitmapFactory.Options();
                bounds.inJustDecodeBounds = true;
                BitmapFactory.decodeFile(jpeg.getAbsolutePath(), bounds);
                assertEquals(size.getWidth(), bounds.outWidth);
                assertEquals(size.getHeight(), bounds.outHeight);
                BitmapFactory.Options preview = new BitmapFactory.Options();
                preview.inSampleSize = 8;
                Bitmap decoded = BitmapFactory.decodeFile(jpeg.getAbsolutePath(), preview);
                assertNotNull("production JPEG decode failed", decoded);
                decoded.recycle();
                JSONObject replay = new JSONObject().put("recipe", new JSONObject(recipe))
                    .put("sourceMetadata", new JSONObject(renderer.metadata(id)))
                    .put("replayToSavedJpegMs", (SystemClock.elapsedRealtimeNanos() - begin) / 1e6)
                    .put("jpegBytes", jpeg.length()).put("width", bounds.outWidth).put("height", bounds.outHeight);
                // Exercise pending -> fsync -> gallery publication and remove
                // only the row created by this test, even when a check fails.
                ContentValues values = new ContentValues();
                values.put(MediaStore.Images.Media.DISPLAY_NAME, "RAWR_HEADLESS_TEST_" + SystemClock.elapsedRealtime() + ".jpg");
                values.put(MediaStore.Images.Media.MIME_TYPE, "image/jpeg");
                values.put(MediaStore.Images.Media.RELATIVE_PATH, "DCIM/RAWRHeadlessTests");
                values.put(MediaStore.Images.Media.IS_PENDING, 1);
                long publishBegin = SystemClock.elapsedRealtimeNanos();
                Uri uri = context.getContentResolver().insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, values);
                assertNotNull(uri);
                try {
                    try (ParcelFileDescriptor fd = context.getContentResolver().openFileDescriptor(uri, "w");
                         FileInputStream input = new FileInputStream(jpeg)) {
                        assertNotNull(fd);
                        // AutoCloseOutputStream owns this test-created provider FD.
                        try (FileOutputStream out = new ParcelFileDescriptor.AutoCloseOutputStream(fd)) {
                            byte[] copy = new byte[256 * 1024];
                            int n;
                            while ((n = input.read(copy)) >= 0) if (n > 0) out.write(copy, 0, n);
                            out.getFD().sync();
                        }
                    }
                    values.clear();
                    values.put(MediaStore.Images.Media.IS_PENDING, 0);
                    assertEquals(1, context.getContentResolver().update(uri, values, null, null));
                    replay.put("providerCopyAndPublishMs", (SystemClock.elapsedRealtimeNanos() - publishBegin) / 1e6);
                    try (android.database.Cursor row = context.getContentResolver().query(uri,
                            new String[]{MediaStore.Images.Media.IS_PENDING, MediaStore.Images.Media.SIZE}, null, null, null)) {
                        assertNotNull(row);
                        assertTrue(row.moveToFirst());
                        assertEquals(0, row.getInt(0));
                        assertEquals(jpeg.length(), row.getLong(1));
                    }
                    try (java.io.InputStream input = context.getContentResolver().openInputStream(uri)) {
                        assertNotNull(input);
                        Bitmap published = BitmapFactory.decodeStream(input, null, preview);
                        assertNotNull("published JPEG decode failed", published);
                        published.recycle();
                    }
                } finally { assertEquals("test MediaStore cleanup", 1, context.getContentResolver().delete(uri, null, null)); }
                try (FileOutputStream report = new FileOutputStream(new File(dir, "production-replay.json"))) {
                    report.write(replay.toString(2).getBytes(java.nio.charset.StandardCharsets.UTF_8));
                }
            } finally { renderer.close(id); }
        }
    }
}
