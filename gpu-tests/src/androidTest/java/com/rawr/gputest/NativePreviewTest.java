package com.rawr.gputest;

import android.Manifest;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Debug;
import android.os.SystemClock;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import com.rawr.camera.integration.NativePreviewEngine;
import java.io.File;
import java.nio.file.Files;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.atomic.AtomicInteger;
import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Test;
import org.junit.runner.RunWith;
import static org.junit.Assert.*;

/** Opt-in production Camera2/Vulkan preview into an offscreen EGL consumer. */
@RunWith(AndroidJUnit4.class)
public final class NativePreviewTest {
    @Test public void previewStopResumeOffscreen() throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        boolean detachSurface = Boolean.parseBoolean(InstrumentationRegistry.getArguments().getString("detachSurface", "false"));
        int cycles = Integer.parseInt(InstrumentationRegistry.getArguments().getString("cycles", "2"));
        int profile = Integer.parseInt(InstrumentationRegistry.getArguments().getString("profile", "2"));
        boolean zeroCopy = Boolean.parseBoolean(InstrumentationRegistry.getArguments().getString("zeroCopy", "true"));
        boolean testUiPause = Boolean.parseBoolean(InstrumentationRegistry.getArguments().getString("testUiPause", "false"));
        int targetFrames = Integer.parseInt(InstrumentationRegistry.getArguments().getString("frames", "30"));
        boolean waveform = Boolean.parseBoolean(InstrumentationRegistry.getArguments().getString("waveform", "false"));
        boolean trace = Boolean.parseBoolean(InstrumentationRegistry.getArguments().getString("trace", "false"));
        boolean highlightReference = Boolean.parseBoolean(InstrumentationRegistry.getArguments().getString("highlightReference", "false"));
        boolean guideReference = Boolean.parseBoolean(InstrumentationRegistry.getArguments().getString("guideReference", "false"));
        assertFalse("Choose one shader reference", highlightReference && guideReference);
        assertTrue(targetFrames >= 30 && targetFrames <= 600);
        assertTrue(cycles >= 2 && cycles <= 10);
        assertTrue(profile >= 0 && profile <= 10 && profile != 1); // Imported LUT needs a separate fixture.
        assertEquals(PackageManager.PERMISSION_GRANTED, context.checkSelfPermission(Manifest.permission.CAMERA));
        File dir = new File(context.getFilesDir(), "gpu-results/native-preview-" + SystemClock.elapsedRealtime());
        assertTrue(dir.mkdirs());
        OffscreenPreviewSurface consumer = new OffscreenPreviewSurface();
        AtomicInteger frames = consumer.frames;
        NativePreviewEngine engine = new NativePreviewEngine();
        long handle = 0;
        Intent service = new Intent(context, HeadlessCameraService.class);
        JSONArray rows = new JSONArray();
        try {
            HeadlessCameraService.resetReady();
            context.startForegroundService(service);
            assertTrue(HeadlessCameraService.awaitReady());
            handle = engine.create(dir.getAbsolutePath(), context.getApplicationInfo().nativeLibraryDir, "");
            assertTrue(handle != 0);
            if (highlightReference) {
                // Run HeadlessGpuTest#highlightControlFlowParity first to stage
                // the pre-change shaders. Override only this test engine's files.
                File reference = new File(context.getFilesDir(), "gpu-results/seed-0");
                Files.copy(new File(reference, "highlight_guide_seed_reference.comp.spv").toPath(),
                    new File(dir, "raw_preview_highlight_guide_seed.comp.spv").toPath(),
                    java.nio.file.StandardCopyOption.REPLACE_EXISTING);
                Files.copy(new File(reference, "highlight_apply_reference.comp.spv").toPath(),
                    new File(dir, "raw_preview_highlight_apply.comp.spv").toPath(),
                    java.nio.file.StandardCopyOption.REPLACE_EXISTING);
            }
            if (guideReference) {
                // guideDenseFieldParity stages the immediately preceding seed
                // and frozen smoothing shader. Keep indirect skipping enabled.
                File reference = new File(context.getFilesDir(), "gpu-results/guide-dense-0-0");
                Files.copy(new File(reference, "highlight_guide_seed_bound_reference.comp.spv").toPath(),
                    new File(dir, "raw_preview_highlight_guide_seed.comp.spv").toPath(),
                    java.nio.file.StandardCopyOption.REPLACE_EXISTING);
                Files.copy(new File(reference, "highlight_guide_smooth_reference.comp.spv").toPath(),
                    new File(dir, "raw_preview_highlight_guide_smooth.comp.spv").toPath(),
                    java.nio.file.StandardCopyOption.REPLACE_EXISTING);
            }
            engine.setAssetManager(handle, context.getAssets());
            engine.setPersistentDiagnosticsEnabled(handle, true);
            engine.setInternalTraceCaptureEnabled(handle, trace);
            assertTrue(engine.setCameraProfile(handle, engine.builtInCameraProfile()));
            // Match SettingsValues' shipped default before camera configuration.
            engine.setExperimentalZeroCopy(handle, zeroCopy);
            engine.setPreferredCameraId(handle, "0");
            engine.setViewfinderDivisor(handle, 2);
            engine.setColorRenderProfile(handle, profile, "");
            if (waveform) engine.setScopePresentationState(handle, new int[]{1, 0, 0}, new int[]{1, 0, 0},
                new int[]{0, 0, 0}, new float[]{.65f, .05f, .30f, .20f, .04f, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
            assertTrue("Offscreen presentation surface rejected", engine.setSurface(handle, consumer.surface(), 0));
            engine.setInitialConfigReady(handle);
            for (int cycle = 0; cycle < cycles; ++cycle) {
                if (cycle > 0 && detachSurface)
                    assertTrue(engine.setSurface(handle, consumer.surface(), 0));
                int first = frames.get();
                long start = SystemClock.elapsedRealtime();
                engine.setPreviewForeground(handle, true);
                while (frames.get() - first < targetFrames && SystemClock.elapsedRealtime() - start < 60_000) {
                    engine.tickCameraSession(handle);
                    Thread.sleep(50);
                }
                JSONObject row = new JSONObject().put("cycle", cycle).put("frames", frames.get() - first)
                    .put("profile", profile)
                    .put("zeroCopy", zeroCopy)
                    .put("highlightReference", highlightReference)
                    .put("guideReference", guideReference)
                    .put("targetFrames", targetFrames).put("waveform", waveform)
                    .put("startToTargetFramesMs", SystemClock.elapsedRealtime() - start)
                    .put("runningPssKiB", Debug.getPss()).put("cameraSnapshot", engine.cameraControlSnapshot(handle));
                rows.put(row);
                Files.write(new File(dir, "lifecycle.json").toPath(), rows.toString(2).getBytes(StandardCharsets.UTF_8));
                assertTrue("Too few production preview frames; inspect native diagnostics", frames.get() - first >= targetFrames);
                assertNull("EGL presentation consumer failed", consumer.failure.get());
                if (testUiPause) {
                    int pauseBeginFrames = frames.get();
                    engine.setUiPresentationActive(handle, false);
                    Thread.sleep(1000);
                    int pausedFrames = frames.get();
                    row.put("uiPauseDrainFrames", pausedFrames - pauseBeginFrames);
                    Thread.sleep(1000);
                    row.put("uiPauseLateFrames", frames.get() - pausedFrames);
                    assertEquals("Presentation continues after UI pause/drain", pausedFrames, frames.get());
                    engine.setUiPresentationActive(handle, true);
                    long resume = SystemClock.elapsedRealtime();
                    while (frames.get() - pausedFrames < 3 && SystemClock.elapsedRealtime() - resume < 5000)
                        Thread.sleep(50);
                    row.put("uiResumeFrames", frames.get() - pausedFrames);
                    row.put("uiResumeWaitMs", SystemClock.elapsedRealtime() - resume);
                    Files.write(new File(dir, "lifecycle.json").toPath(), rows.toString(2).getBytes(StandardCharsets.UTF_8));
                    assertTrue("Presentation did not resume", frames.get() - pausedFrames >= 3);
                }
                long stopBegin = SystemClock.elapsedRealtime();
                engine.setPreviewForeground(handle, false);
                engine.tickCameraSession(handle);
                row.put("stopTickMs", SystemClock.elapsedRealtime() - stopBegin);
                if (detachSurface) assertTrue(engine.setSurface(handle, null, 0));
                row.put("surfaceDetached", detachSurface);
                row.put("stopAndDetachMs", SystemClock.elapsedRealtime() - stopBegin);
                Thread.sleep(1000); // Allow submitted frames to drain.
                int stoppedFrames = frames.get();
                long cpu = android.os.Process.getElapsedCpuTime();
                Thread.sleep(1000);
                row.put("idleCpuMsOver1s", android.os.Process.getElapsedCpuTime() - cpu)
                    .put("stoppedPssKiB", Debug.getPss()).put("lateFrames", frames.get() - stoppedFrames);
                Files.write(new File(dir, "lifecycle.json").toPath(), rows.toString(2).getBytes(StandardCharsets.UTF_8));
                assertEquals("Preview continues presenting after stop/drain", stoppedFrames, frames.get());
            }
        } finally {
            try {
                if (handle != 0) {
                    try {
                        engine.setPreviewForeground(handle, false);
                        engine.tickCameraSession(handle);
                        engine.setSurface(handle, null, 0);
                    } finally { engine.destroy(handle); }
                }
            } finally {
                context.stopService(service);
                consumer.close();
            }
        }
    }
}
