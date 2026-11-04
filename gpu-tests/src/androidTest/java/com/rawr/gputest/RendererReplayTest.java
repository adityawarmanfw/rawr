package com.rawr.gputest;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.os.Debug;
import android.os.ParcelFileDescriptor;
import android.os.SystemClock;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import com.rawr.camera.renderer.RendererNative;
import java.io.File;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.security.MessageDigest;
import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Test;
import org.junit.runner.RunWith;
import static org.junit.Assert.*;

/** Camera-free, cold-cache replay of the captured sensor fixture. */
@RunWith(AndroidJUnit4.class)
public final class RendererReplayTest {
    @Test public void coldReplay() throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        android.os.Bundle args = InstrumentationRegistry.getArguments();
        String label = args.getString("label", "replay");
        assertTrue(label.matches("[A-Za-z0-9_-]+"));
        int samples = Integer.parseInt(args.getString("samples", "3"));
        boolean ultraHdr = Boolean.parseBoolean(args.getString("ultraHdr", "false"));
        assertTrue(samples > 0 && samples <= 30);
        // Recipes use case-sensitive enum names. Fail the benchmark rather
        // than silently measuring the renderer's unknown-profile fallback.
        String profile = args.getString("profile", "RawrBase");
        assertTrue("Use an exact built-in recipe profile name", java.util.Arrays.asList(
            "RawrBase", "SRgb", "Rec709", "AgX").contains(profile));
        File source = new File(context.getFilesDir(), "gpu-results/camera-0/capture-0.dng");
        assertTrue("Run HeadlessCameraTest first", source.isFile());
        File root = new File(context.getFilesDir(), "gpu-results/" + label);
        assertTrue("Use a fresh label to avoid cache reuse", root.mkdir());
        JSONArray reports = new JSONArray();
        for (int i = 0; i < samples; ++i) {
            File dir = new File(root, "sample-" + i);
            assertTrue(dir.mkdir());
            File input = new File(dir, "source.dng");
            // Fixture copy is outside measured render time. Each unique source
            // path gives the production renderer an empty linear-cache key.
            Files.copy(source.toPath(), input.toPath(), StandardCopyOption.REPLACE_EXISTING);
            JSONObject recipeJson = new JSONObject().put("demosaicAlgorithm", args.getString("demosaic", "Rcd"))
                .put("colorRenderProfile", profile)
                .put("highlightReconstructionEnabled", true)
                .put("highlightReconstructionMethod", Integer.parseInt(args.getString("highlightMethod", "1")))
                .put("fccSteps", 2).put("defringeEnabled", true)
                .put("tone", new JSONObject().put("jpegQuality", 98).put("jpegChromaSubsamplingId", "jpeg.422"));
            if (ultraHdr) recipeJson.put("ultraHdrEnabled", true);
            String recipe = recipeJson.toString();
            RendererNative renderer = new RendererNative();
            File jpeg = new File(dir, "output.jpg");
            long begin = SystemClock.elapsedRealtimeNanos();
            long handle = renderer.open(input.getAbsolutePath(), dir.getAbsolutePath(),
                context.getApplicationInfo().nativeLibraryDir, context.getAssets());
            assertTrue(handle != 0);
            try (ParcelFileDescriptor output = ParcelFileDescriptor.open(jpeg,
                    ParcelFileDescriptor.MODE_CREATE | ParcelFileDescriptor.MODE_TRUNCATE | ParcelFileDescriptor.MODE_READ_WRITE)) {
                renderer.render(handle, recipe, 4080, 3060, output.getFd(), null, 4080, 3060);
                output.getFileDescriptor().sync();
            } finally { renderer.close(handle); }
            double renderMs = (SystemClock.elapsedRealtimeNanos() - begin) / 1e6;
            long postClosePss = Debug.getPss();
            Bitmap bitmap = BitmapFactory.decodeFile(jpeg.getAbsolutePath());
            assertNotNull(bitmap);
            assertEquals(4080, bitmap.getWidth()); assertEquals(3060, bitmap.getHeight());
            if (ultraHdr) {
                byte[] header = new byte[8192];
                try (java.io.FileInputStream stream = new java.io.FileInputStream(jpeg)) {
                    int count = stream.read(header);
                    assertTrue(count > 0);
                    String markers = new String(header, 0, count, StandardCharsets.ISO_8859_1);
                    assertTrue("UltraHDR export must retain MPF", markers.contains("MPF\u0000"));
                    assertTrue("UltraHDR export must retain ISO gain-map metadata", markers.contains("urn:iso:std:iso:ts:21496:-1"));
                }
            }
            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            int[] row = new int[bitmap.getWidth()];
            ByteBuffer bytes = ByteBuffer.allocate(row.length * 4);
            for (int y = 0; y < bitmap.getHeight(); ++y) {
                bitmap.getPixels(row, 0, row.length, 0, y, row.length, 1);
                bytes.clear(); bytes.asIntBuffer().put(row);
                digest.update(bytes.array());
            }
            bitmap.recycle();
            StringBuilder hex = new StringBuilder();
            for (byte b : digest.digest()) hex.append(String.format(java.util.Locale.ROOT, "%02x", b & 255));
            reports.put(new JSONObject().put("sample", i).put("recipe", new JSONObject(recipe))
                .put("openRenderFsyncCloseMs", renderMs).put("postClosePssKiB", postClosePss)
                .put("jpegBytes", jpeg.length()).put("decodedArgbSha256", hex.toString()));
            Files.write(new File(root, "results.json").toPath(), reports.toString(2).getBytes(java.nio.charset.StandardCharsets.UTF_8));
        }
    }
}
