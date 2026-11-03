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
    @Test public void guideDenseFieldParity() throws Exception {
        File root = new File(InstrumentationRegistry.getInstrumentation().getTargetContext().getFilesDir(), "gpu-results");
        for (int scene = 0; scene < 7; ++scene) {
            for (int variant = 0; variant < (scene == 0 ? 5 : 4); ++variant) {
                java.util.ArrayList<String> args = new java.util.ArrayList<>(java.util.Arrays.asList(
                    "--method", "0", "--frames", scene == 0 ? "90" : "21", "--fps", "30",
                    "--width", scene == 0 ? "4080" : scene == 1 || scene == 4 ? "638" : scene == 3 || scene == 5 ? "8" : scene == 6 ? "258" : "514",
                    "--height", scene == 0 ? "3064" : scene == 1 || scene == 4 ? "514" : scene == 3 || scene == 6 ? "8" : scene == 5 ? "258" : "386",
                    scene == 1 ? "--stress-only" : "--normal-only", scene == 4 ? "--empty-guide" : "--force-clip", "--stage-timing",
                    "--wb-r", "2.1", "--wb-b", "0.8", "--dump", "linear.rgba16f"));
                if (scene == 0 || scene == 1 || scene == 2 || scene == 4) {
                    args.add("--lsc"); args.add("17x13");
                }
                if (scene == 2) args.add("--toggle-guide");
                if (scene == 3 || scene == 5 || scene == 6) args.add("--flat-guide");
                if (variant == 0 || variant == 4) {
                    args.add("--seed-shader"); args.add("highlight_guide_seed_bound_reference.comp.spv");
                }
                if (variant == 1) { args.add("--seed-shader"); args.add("highlight_guide_seed_channel_only.comp.spv"); }
                // Hold the existing indirect controller constant. The frozen
                // generic shader has no specialization ID, so it remains the
                // preceding smoothing kernel even when the fast pipeline binds.
                args.add("--guide-control"); args.add("highlight_guide_control.comp.spv");
                if (variant != 3) { args.add("--smooth-shader"); args.add("highlight_guide_smooth_reference.comp.spv"); }
                run("preview", "guide-dense-" + scene + "-" + variant, args.toArray(new String[0]));
                org.junit.Assert.assertArrayEquals("Dense guide must preserve pixels, scene=" + scene + " variant=" + variant,
                    Files.readAllBytes(new File(root, "guide-dense-" + scene + "-0/linear.rgba16f").toPath()),
                    Files.readAllBytes(new File(root, "guide-dense-" + scene + "-" + variant + "/linear.rgba16f").toPath()));
                if (variant == 3) {
                    String log = new String(Files.readAllBytes(new File(root, "guide-dense-" + scene + "-3/native.txt").toPath()), StandardCharsets.UTF_8);
                    org.junit.Assert.assertTrue(log.contains("GUIDE_INDIRECT_ARGS"));
                    if (scene == 4) org.junit.Assert.assertTrue(log.contains("propagateX=0 smoothX=0"));
                    else org.junit.Assert.assertFalse("Fixture must exercise dense smoothing, scene=" + scene, log.contains("propagateX=0"));
                }
            }
        }
    }
    @Test public void optimizedGuideCompilerParity() throws Exception {
        File root = new File(InstrumentationRegistry.getInstrumentation().getTargetContext().getFilesDir(), "gpu-results");
        for (int variant = 0; variant < 4; ++variant) {
            java.util.ArrayList<String> args = new java.util.ArrayList<>(java.util.Arrays.asList(
                "--method", "0", "--frames", "60", "--fps", "30", "--width", "4080", "--height", "3064",
                "--normal-only", "--force-clip", "--lsc", "17x13", "--wb-r", "2.1", "--wb-b", "0.8",
                "--dump", "linear.rgba16f"));
            if ((variant & 1) != 0) {
                args.add("--propagate-shader"); args.add("highlight_guide_propagate_optimized.comp.spv");
            }
            if ((variant & 2) != 0) {
                args.add("--smooth-shader"); args.add("highlight_guide_smooth_optimized.comp.spv");
            }
            run("preview", "guide-compiler-" + variant, args.toArray(new String[0]));
            org.junit.Assert.assertArrayEquals("Compiler variant must preserve recovered pixels, variant=" + variant,
                Files.readAllBytes(new File(root, "guide-compiler-0/linear.rgba16f").toPath()),
                Files.readAllBytes(new File(root, "guide-compiler-" + variant + "/linear.rgba16f").toPath()));
        }
    }
    @Test public void indirectGuideParity() throws Exception {
        File root = new File(InstrumentationRegistry.getInstrumentation().getTargetContext().getFilesDir(), "gpu-results");
        for (int scene = 0; scene < 6; ++scene) {
            for (int variant = 0; variant < 2; ++variant) {
                java.util.ArrayList<String> args = new java.util.ArrayList<>(java.util.Arrays.asList(
                    "--method", "0", "--mask-shader", variant == 0 ? "raw_preview_cfa_reference.comp.spv" : "raw_preview_cfa_state.comp.spv",
                    "--frames", scene == 3 ? "12" : scene == 4 ? "15" : "30", "--fps", "30",
                    "--width", scene == 3 || scene == 4 ? "512" : scene == 5 ? "638" : "4080",
                    "--height", scene == 3 || scene == 4 ? "386" : scene == 5 ? "514" : "3064",
                    scene == 5 ? "--stress-only" : "--normal-only", "--lsc", "17x13",
                    "--wb-r", "2.1", "--wb-b", "0.8", "--dump", "linear.rgba16f"));
                if (scene == 0) args.add("--no-clip");
                if (scene == 1) args.add("--empty-guide");
                if (scene == 2) args.add("--force-clip");
                if (scene == 3 || scene == 4) args.add("--toggle-guide");
                if (variant == 1) {
                    args.add("--guide-control");
                    args.add("highlight_guide_control.comp.spv");
                }
                run("preview", "indirect-" + scene + "-" + variant, args.toArray(new String[0]));
            }
            org.junit.Assert.assertArrayEquals("Indirect guide must preserve recovered image, scene=" + scene,
                Files.readAllBytes(new File(root, "indirect-" + scene + "-0/linear.rgba16f").toPath()),
                Files.readAllBytes(new File(root, "indirect-" + scene + "-1/linear.rgba16f").toPath()));
            String log = new String(Files.readAllBytes(new File(root, "indirect-" + scene + "-1/native.txt").toPath()), StandardCharsets.UTF_8);
            if (scene == 0) org.junit.Assert.assertTrue(log.contains("seedX=0 propagateX=0 smoothX=0 applyX=0"));
            if (scene == 1 || scene == 3) org.junit.Assert.assertTrue(log.contains("propagateX=0 smoothX=0"));
            if (scene == 1 || scene == 3) org.junit.Assert.assertFalse("Clipped fixture did not exercise recovery", log.contains("applyX=0"));
            if (scene == 2 || scene == 4) org.junit.Assert.assertFalse("Seeded fixture did not exercise propagation", log.contains("propagateX=0"));
        }
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
