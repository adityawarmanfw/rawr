package com.rawr.gputest;

import android.content.Context;
import android.os.Debug;
import android.os.SystemClock;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import org.junit.Test;
import org.junit.runner.RunWith;
import static org.junit.Assert.assertEquals;

/** No ActivityScenario, SurfaceView, camera permission or app launch. */
@RunWith(AndroidJUnit4.class)
public class HeadlessGpuTest {
    private static native int runNative(String name, String[] args, String outputPath);
    private static native int agxPresetCount();
    private void run(String name, String label, String... args) throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        System.load(new File(context.getApplicationInfo().nativeLibraryDir, "librawr_gpu_tests.so").getAbsolutePath());
        File dir = new File(context.getFilesDir(), "gpu-results/" + label);
        if (!dir.exists() && !dir.mkdirs()) throw new IllegalStateException("cannot create report directory");
        long begin = SystemClock.elapsedRealtimeNanos();
        int result = runNative(name, args, dir.getAbsolutePath());
        String report = "case=" + label + " exit=" + result + " wallMs=" +
            (SystemClock.elapsedRealtimeNanos() - begin) / 1e6 + " processPssKiB=" + Debug.getPss() + "\n";
        Files.write(new File(dir, "summary.txt").toPath(), report.getBytes(StandardCharsets.UTF_8));
        assertEquals(report + " See files/gpu-results/" + label + "/native.txt", 0, result);
    }
    @Test public void rawRgbAllBayerPatterns() throws Exception { run("rgb", "raw-rgb"); }
    @Test public void exactCfaClassification() throws Exception { run("cfa", "cfa"); }
    @Test public void cachedReadbackExactBytes() throws Exception { run("readback", "readback"); }
    @Test public void toneOriginalCstReference() throws Exception {
        // The legacy CPU oracle predates current tone controls. Compare GPU
        // output with the original CST arithmetic using the current controls.
        run("tonemap", "tone-reference", "--shader", "tonemap.comp.spv", "--reference-shader", "tonemap-reference.comp.spv",
            "--width", "512", "--height", "384", "--warmup", "3", "--iterations", "10");
    }
    @Test public void toneProfilesPerformance() throws Exception {
        // Existing/RAWR transform plus every AgX preset. Deterministic GPU
        // outputs and timing, not a claim of CPU parity for non-neutral looks.
        run("tonemap", "tone-rawr", "--shader", "tonemap.comp.spv", "--width", "1020", "--height", "765",
            "--bench-only", "--warmup", "3", "--iterations", "20");
        for (int transform = 1; transform < 4; ++transform)
            run("tonemap", "direct-" + transform, "--shader", "tonemap.comp.spv", "--width", "1020", "--height", "765",
                "--render-transform", Integer.toString(transform), "--bench-only", "--warmup", "3", "--iterations", "20");
        for (int preset = 0; preset < agxPresetCount(); ++preset)
            run("tonemap", "agx-" + preset, "--shader", "tonemap.comp.spv", "--width", "1020", "--height", "765",
                "--render-transform", "4", "--agx-preset", Integer.toString(preset), "--bench-only", "--warmup", "3", "--iterations", "20");
    }
    @Test public void rawPreviewHighlightMethods() throws Exception {
        for (int method = 0; method < 2; ++method)
            run("preview", "preview-" + method, "--method", Integer.toString(method),
                "--frames", "300", "--fps", "30", "--normal-only");
    }
    @Test public void previewToneBypassSwitching() throws Exception {
        run("preview", "tone-bypass", "--method", "1", "--bypass-tone", "--width", "1024", "--height", "768",
            "--frames", "12", "--fps", "30", "--normal-only", "--dump", "linear.rgba16f");
        run("preview", "tone-switch", "--method", "1", "--toggle-tone", "--width", "1024", "--height", "768",
            "--frames", "12", "--fps", "30", "--normal-only", "--dump", "linear.rgba16f");
        File root = new File(InstrumentationRegistry.getInstrumentation().getTargetContext().getFilesDir(), "gpu-results");
        org.junit.Assert.assertArrayEquals("Tone presentation must not change recovered linear pixels",
            Files.readAllBytes(new File(root, "tone-bypass/linear.rgba16f").toPath()),
            Files.readAllBytes(new File(root, "tone-switch/linear.rgba16f").toPath()));
    }
    @Test public void highlightControlFlowParity() throws Exception {
        File root = new File(InstrumentationRegistry.getInstrumentation().getTargetContext().getFilesDir(), "gpu-results");
        int sceneCount = Integer.parseInt(InstrumentationRegistry.getArguments().getString("highlightScenes", "3"));
        org.junit.Assert.assertTrue(sceneCount >= 1 && sceneCount <= 3);
        for (int scene = 0; scene < sceneCount; ++scene) {
            for (int variant = 0; variant < 2; ++variant) {
                run("preview", "seed-" + (scene * 2 + variant), "--method", "0",
                    "--seed-shader", variant == 0 ? "highlight_guide_seed_reference.comp.spv" : "raw_preview_highlight_guide_seed.comp.spv",
                    "--apply-shader", variant == 0 ? "highlight_apply_reference.comp.spv" : "raw_preview_highlight_apply.comp.spv",
                    "--frames", "60", "--fps", "30", "--width", scene == 2 ? "638" : "4080",
                    "--height", scene == 2 ? "514" : "3064", scene == 1 ? "--stress-only" : "--normal-only",
                    "--wb-r", scene == 0 ? "1" : "2.1", "--wb-b", scene == 0 ? "1" : "0.8",
                    "--lsc", "17x13", "--dump", "linear.rgba16f");
            }
            org.junit.Assert.assertArrayEquals("Guide changes must preserve recovered image bytes, scene=" + scene,
                Files.readAllBytes(new File(root, "seed-" + (scene * 2) + "/linear.rgba16f").toPath()),
                Files.readAllBytes(new File(root, "seed-" + (scene * 2 + 1) + "/linear.rgba16f").toPath()));
        }
    }
    @Test public void haloClassificationParity() throws Exception {
        File root = new File(InstrumentationRegistry.getInstrumentation().getTargetContext().getFilesDir(), "gpu-results");
        for (int method = 0; method < 2; ++method) {
            for (int variant = 0; variant < 2; ++variant) {
                run("preview", "halo-" + method + "-" + variant, "--method", Integer.toString(method),
                    "--mask-shader", variant == 0 ? "raw_preview_cfa_full_halo.comp.spv" : "raw_preview_cfa_state.comp.spv",
                    "--frames", "60", "--fps", "30", "--width", "4080", "--height", "3064",
                    "--normal-only", "--lsc", "17x13", "--dump", "linear.rgba16f");
            }
            org.junit.Assert.assertArrayEquals("Skipping halo warning classification must preserve recovered pixels",
                Files.readAllBytes(new File(root, "halo-" + method + "-0/linear.rgba16f").toPath()),
                Files.readAllBytes(new File(root, "halo-" + method + "-1/linear.rgba16f").toPath()));
        }
        run("cfa", "halo-cfa", "--mask-shader", "raw_preview_cfa_state.comp.spv", "--width", "512", "--height", "384");
    }
    @Test public void optimizedPreviewCompilerParity() throws Exception {
        for (int method = 0; method < 2; ++method) {
            for (int variant = 0; variant < 2; ++variant) {
                run("preview", "compiler-" + method + "-" + variant, "--method", Integer.toString(method),
                    "--mask-shader", variant == 0 ? "raw_preview_cfa_state.comp.spv" : "raw_preview_cfa_optimized.comp.spv",
                    "--frames", "30", "--fps", "30", "--width", "4080", "--height", "3064",
                    "--normal-only", "--lsc", "17x13", "--dump", "linear.rgba16f");
            }
            File root = new File(InstrumentationRegistry.getInstrumentation().getTargetContext().getFilesDir(), "gpu-results");
            org.junit.Assert.assertArrayEquals("Compiler optimization must preserve recovered linear bytes",
                Files.readAllBytes(new File(root, "compiler-" + method + "-0/linear.rgba16f").toPath()),
                Files.readAllBytes(new File(root, "compiler-" + method + "-1/linear.rgba16f").toPath()));
        }
        run("cfa", "compiler-cfa", "--mask-shader", "raw_preview_cfa_optimized.comp.spv", "--width", "512", "--height", "384");
    }
    @Test public void optimizedStridedBufferParity() throws Exception {
        File root = new File(InstrumentationRegistry.getInstrumentation().getTargetContext().getFilesDir(), "gpu-results");
        byte[] reference = null;
        for (int stride : new int[]{0, 4096, 4081}) {
            for (int variant = 0; variant < 2; ++variant) {
                String label = "buffer-" + stride + "-" + variant;
                run("preview", label, "--method", "1", "--buffer-stride", Integer.toString(stride),
                    "--mask-shader", variant == 0 ? "raw_preview_cfa_state.comp.spv" : "raw_preview_cfa_optimized.comp.spv",
                    "--frames", "30", "--fps", "30", "--width", "4080", "--height", "3064",
                    "--normal-only", "--lsc", "17x13", "--dump", "linear.rgba16f");
                byte[] actual = Files.readAllBytes(new File(root, label + "/linear.rgba16f").toPath());
                if (reference == null) reference = actual;
                else org.junit.Assert.assertArrayEquals("Strided buffer/compiler output differs from image input", reference, actual);
            }
        }
    }
}
