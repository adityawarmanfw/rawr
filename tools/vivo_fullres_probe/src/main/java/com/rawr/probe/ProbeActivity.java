package com.rawr.probe;

import android.app.Activity;
import android.graphics.ImageFormat;
import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CaptureFailure;
import android.hardware.camera2.CaptureRequest;
import android.hardware.camera2.CaptureResult;
import android.hardware.camera2.TotalCaptureResult;
import android.hardware.camera2.params.OutputConfiguration;
import android.hardware.camera2.params.SessionConfiguration;
import android.media.Image;
import android.media.ImageReader;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.SystemClock;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.PrintWriter;
import java.lang.reflect.Array;
import java.lang.reflect.Method;
import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.Executor;

/**
 * Headless probe for vivo full-size RAW sensor modes, driven by adb:
 * <pre>
 * am start -n &lt;pkg&gt;/com.rawr.probe.ProbeActivity --es mode dump
 * am start -n &lt;pkg&gt;/com.rawr.probe.ProbeActivity --es mode capture --es cam 5 --ei row 1 --ei w 8160 --ei h 6144 [--ei frames 3] [--ez lease true]
 * </pre>
 * Output goes to getExternalFilesDir(null)/&lt;run&gt;/ and the log tag RAWRPROBE; "PROBE DONE" ends every run.
 */
public final class ProbeActivity extends Activity {
    private static final String TAG = "RAWRPROBE";
    private static final int SDK_KEY = 40, VCF_KEY = 33;

    private HandlerThread thread;
    private Handler handler;

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        thread = new HandlerThread("probe");
        thread.start();
        handler = new Handler(thread.getLooper());
        final String mode = stringExtra("mode", "dump");
        handler.post(() -> {
            try {
                if ("capture".equals(mode)) capture();
                else if ("stock".equals(mode)) stockCapture();
                else if ("methods".equals(mode)) methods(getIntent().getStringExtra("cls"));
                else if ("lease".equals(mode)) {
                    // put the vivo values back by hand after a killed run: --ei sdk 0 --ei vcf 2
                    Provider p = provider();
                    p.set(VCF_KEY, getIntent().getIntExtra("vcf", 2));
                    p.set(SDK_KEY, getIntent().getIntExtra("sdk", 0));
                    SystemClock.sleep(100);
                    try (PrintWriter out = new PrintWriter(new File(runDir("lease"), "lease.txt"))) {
                        out.println("lease=" + p.get(SDK_KEY) + "/" + p.get(VCF_KEY));
                    }
                }
                else dump();
            } catch (Throwable t) {
                Log.e(TAG, "probe failed", t);
            }
            Log.i(TAG, "PROBE DONE");
            runOnUiThread(this::finish);
        });
    }

    @Override protected void onDestroy() {
        thread.quitSafely();
        super.onDestroy();
    }

    private String stringExtra(String key, String fallback) {
        String v = getIntent().getStringExtra(key);
        return v == null ? fallback : v;
    }

    private File runDir(String name) {
        File dir = new File(getExternalFilesDir(null), name);
        dir.mkdirs();
        return dir;
    }

    // ---- characteristics dump ------------------------------------------------------------------------------------------

    private void dump() throws Exception {
        Provider lease = null;
        if (getIntent().getBooleanExtra("lease", false)) {
            lease = provider();
            lease.set(VCF_KEY, 2);
            lease.set(SDK_KEY, 1);
            SystemClock.sleep(100);
        }
        try {
            dumpTo(runDir(lease != null ? "dump_lease" : "dump"));
        } finally {
            if (lease != null) { lease.set(VCF_KEY, 2); lease.set(SDK_KEY, 0); }
        }
    }

    private void dumpTo(File dir) throws Exception {
        CameraManager manager = getSystemService(CameraManager.class);
        List<String> ids = new ArrayList<>();
        Collections.addAll(ids, manager.getCameraIdList());
        for (int i = 0; i < 16; i++) if (!ids.contains(String.valueOf(i))) ids.add(String.valueOf(i));
        for (String id : ids) {
            CameraCharacteristics c;
            try {
                c = manager.getCameraCharacteristics(id);
            } catch (Exception e) {
                continue;
            }
            try (PrintWriter out = new PrintWriter(new File(dir, "chars_" + id + ".txt"))) {
                for (CameraCharacteristics.Key<?> key : c.getKeys()) {
                    Object v;
                    try { v = c.get(key); } catch (Exception e) { v = "<" + e + ">"; }
                    out.println(key.getName() + " = " + format(v));
                }
                for (String name : new String[]{"org.quic.camera2.sensormode.info.SensorModeTable"}) {
                    try {
                        out.println(name + " = " + format(c.get(new CameraCharacteristics.Key<>(name, int[].class))));
                    } catch (Exception e) {
                        out.println(name + " = <" + e + ">");
                    }
                }
                out.println("## request keys");
                for (CaptureRequest.Key<?> key : c.getAvailableCaptureRequestKeys()) out.println(key.getName());
                out.println("## session keys");
                for (CaptureRequest.Key<?> key : c.getAvailableSessionKeys()) out.println(key.getName());
            }
            Log.i(TAG, "dumped camera " + id);
        }
        try {
            Provider p = provider();
            Log.i(TAG, "VivoCameraManager key40=" + p.get(SDK_KEY) + " key33=" + p.get(VCF_KEY));
        } catch (Throwable t) {
            Log.w(TAG, "VivoCameraManager unavailable: " + t);
        }
    }

    private static String format(Object v) {
        if (v == null || !v.getClass().isArray()) return String.valueOf(v);
        int n = Array.getLength(v);
        StringBuilder b = new StringBuilder("[").append(n).append("] ");
        for (int i = 0; i < n; i++) {
            if (i > 0) b.append(',');
            b.append(format(Array.get(v, i)));
        }
        return b.toString();
    }

    /** Every declared method, constructor and field (with static values) of a class, into methods/<class>.txt. */
    private void methods(String name) throws Exception {
        if (name == null) name = "android.hardware.vivocamera.VivoCameraManager";
        Class<?> c = Class.forName(name);
        try (PrintWriter out = new PrintWriter(new File(runDir("methods"), name + ".txt"))) {
            out.println("class " + c + " extends " + c.getSuperclass());
            for (Class<?> i : c.getInterfaces()) out.println("implements " + i);
            for (java.lang.reflect.Field f : c.getDeclaredFields()) {
                String v = "";
                if (java.lang.reflect.Modifier.isStatic(f.getModifiers())) {
                    try { f.setAccessible(true); v = " = " + format(f.get(null)); } catch (Throwable t) { v = " = <" + t + ">"; }
                }
                out.println("field " + f + v);
            }
            for (java.lang.reflect.Constructor<?> k : c.getDeclaredConstructors()) out.println("ctor " + k);
            for (Method m : c.getDeclaredMethods()) out.println("method " + m);
            for (Class<?> n : c.getDeclaredClasses()) out.println("inner " + n);
        }
    }

    // ---- the stock camera's 200 MP RAW route (captured with cmd media.camera watch, 2026-10-17) ------------------------

    @SuppressWarnings("unchecked")
    private static <T> void copyKey(CaptureRequest.Builder from, CaptureRequest.Builder to, CaptureRequest.Key<T> key) {
        T v = from.get(key);
        if (v != null) to.set(key, v);
    }

    private static void setInt(CaptureRequest.Builder b, String name, int... v) {
        b.set(new CaptureRequest.Key<>(name, int[].class), v);
    }

    /** 200 MP: ultra 1, streamsUsage [4 1 0 5]; 50 MP: ultra 0, streamsUsage [5 0 1 0 5 0] (--ei ultra, --es usage). */
    private int stockUltra = 1;
    private int[] stockUsage = {4, 1, 0, 5};

    /** The stock app's session keys of its tele high-res RAW sessions (no forceSensorMode: the HAL switches for the still). */
    private int stockAiHighRes = -1;  // --ei aihr
    private float stockZoom = 0f;  // --ef zoom: CONTROL_ZOOM_RATIO on every request (the stock app's 50 MP tele is camera 2 at 3.7)

    private boolean stockMinKeys = false;  // --ez minkeys: only ai_highresolution (no pro-mode keys)

    private java.util.Set<String> stockDrop = new java.util.HashSet<>();  // --es dropkeys a,b: stock keys left out

    // --es pvdrop a,b: keys also left out of the repeating preview request only; --es sessdrop: of the session only
    private java.util.Set<String> previewDrop = new java.util.HashSet<>(), sessionDrop = new java.util.HashSet<>();

    private void previewKeys(CaptureRequest.Builder b) { keysWithout(b, previewDrop); }
    private void sessionKeys(CaptureRequest.Builder b) { keysWithout(b, sessionDrop); }
    private void keysWithout(CaptureRequest.Builder b, java.util.Set<String> extra) {
        java.util.Set<String> saved = stockDrop;
        stockDrop = new java.util.HashSet<>(saved);
        stockDrop.addAll(extra);
        if (stockDrop.isEmpty()) stockDrop.add("-");  // force the per-key path
        try { stockSessionKeys(b); } finally { stockDrop = saved; }
    }

    private java.util.Map<String, Integer> extraKeys = new java.util.LinkedHashMap<>();  // --es setkeys a=1,b=2

    private void stockSessionKeys(CaptureRequest.Builder b) {
        for (java.util.Map.Entry<String, Integer> e : extraKeys.entrySet()) setInt(b, e.getKey(), e.getValue());
        if (!stockDrop.isEmpty()) {
            java.util.Map<String, int[]> keys = new java.util.LinkedHashMap<>();
            keys.put("vivo.control.ultra_highresolution", new int[]{stockUltra});
            keys.put("vivo.control.is_pro_mode", new int[]{1});
            keys.put("vivo.control.is_ProRaw_on", new int[]{2});
            keys.put("com.vivo.current_ui_module", new int[]{74});
            keys.put("vivo.control.echo.mode", new int[]{2});
            keys.put("vivo.control.scenery.subfunction", new int[]{5});
            keys.put("vivo.control.session.uiSwitch_state", new int[]{2});
            keys.put("vivo.control.streamsUsage", stockUsage);
            for (java.util.Map.Entry<String, int[]> e : keys.entrySet()) {
                String shortName = e.getKey().substring(e.getKey().lastIndexOf('.') + 1);
                if (!stockDrop.contains(shortName)) setInt(b, e.getKey(), e.getValue());
            }
            if (!stockDrop.contains("camera_scenery_pro"))
                setInt(b, "vivo.control.camera_scenery_pro", 1);  // as the full set (worked for stills)
            if (stockAiHighRes >= 0) setInt(b, "vivo.control.ai_highresolution", stockAiHighRes);
            return;
        }
        if (stockMinKeys) {
            if (stockAiHighRes >= 0) setInt(b, "vivo.control.ai_highresolution", stockAiHighRes);
            return;
        }
        if (stockZoom > 0f) {
            b.set(CaptureRequest.CONTROL_ZOOM_RATIO, stockZoom);
            b.set(new CaptureRequest.Key<>("vivo.control.zoom_ratio", float[].class), new float[]{stockZoom});
        }
        setInt(b, "vivo.control.ultra_highresolution", stockUltra);
        if (stockAiHighRes >= 0) setInt(b, "vivo.control.ai_highresolution", stockAiHighRes);  // the stock 50 MP flag
        setInt(b, "vivo.control.is_pro_mode", 1);
        setInt(b, "vivo.control.is_ProRaw_on", 2);
        setInt(b, "com.vivo.current_ui_module", 74);
        setInt(b, "vivo.control.echo.mode", 2);
        setInt(b, "vivo.control.camera_scenery_pro", 1);
        setInt(b, "vivo.control.scenery.subfunction", 5);
        setInt(b, "vivo.control.session.uiSwitch_state", 2);
        setInt(b, "vivo.control.streamsUsage", stockUsage);
    }

    /**
     * --es mode stock --es cam 5 [--ei iso 50] [--el exp 50000000]: the four stock streams (YUV 1440x1080 preview, RAW and YUV
     * at the full size, RAW at the binned size), AF and AE on the preview, then one STILL_CAPTURE to both RAW streams.
     */
    private void stockCapture() throws Exception {
        final String cam = stringExtra("cam", "5");
        final int fw = getIntent().getIntExtra("w", 16320), fh = getIntent().getIntExtra("h", 12288);
        final int bw = getIntent().getIntExtra("bw", 4080), bh = getIntent().getIntExtra("bh", 3072);
        final int iso = getIntent().getIntExtra("iso", 50);
        final long exp = getIntent().getLongExtra("exp", 50_000_000L);
        final boolean lease = getIntent().getBooleanExtra("lease", true);
        stockUltra = getIntent().getIntExtra("ultra", 1);
        stockZoom = getIntent().getFloatExtra("zoom", 0f);
        stockAiHighRes = getIntent().getIntExtra("aihr", -1);
        stockMinKeys = getIntent().getBooleanExtra("minkeys", false);
        for (String kv : stringExtra("setkeys", "").split(",")) {
            int eq = kv.indexOf('=');
            if (eq > 0) extraKeys.put(kv.substring(0, eq), Integer.parseInt(kv.substring(eq + 1)));
        }
        String drop = stringExtra("dropkeys", "");
        if (!drop.isEmpty()) stockDrop.addAll(java.util.Arrays.asList(drop.split(",")));
        String pvd = stringExtra("pvdrop", ""), ssd = stringExtra("sessdrop", "");
        if (!pvd.isEmpty()) previewDrop.addAll(java.util.Arrays.asList(pvd.split(",")));
        if (!ssd.isEmpty()) sessionDrop.addAll(java.util.Arrays.asList(ssd.split(",")));
        final String usage = stringExtra("usage", stockUltra == 1 ? "4,1,0,5" : "5,0,1,0,5,0");
        stockUsage = java.util.Arrays.stream(usage.split(",")).mapToInt(Integer::parseInt).toArray();
        final File dir = runDir("stock_cam" + cam);
        for (File f : dir.listFiles()) f.delete();
        final PrintWriter meta = new PrintWriter(new File(dir, "meta.txt"));
        meta.println("stock route cam=" + cam + " full=" + fw + "x" + fh + " binned=" + bw + "x" + bh + " iso=" + iso + " exp=" + exp + " lease=" + lease);
        Provider p = null;
        int oldSdk = 0, oldVcf = 0;
        if (lease) {
            p = provider();
            oldSdk = p.get(SDK_KEY);
            oldVcf = p.get(VCF_KEY);
            // --ez sdkonly true: only key 40 (useVivoCamera); cameraserver sets 33 itself at open
            if (!getIntent().getBooleanExtra("sdkonly", false)) p.set(VCF_KEY, 2);
            p.set(SDK_KEY, 1);
            SystemClock.sleep(100);
            meta.println("lease before=" + oldSdk + "/" + oldVcf + " after=" + p.get(SDK_KEY) + "/" + p.get(VCF_KEY)
                    + (getIntent().getBooleanExtra("sdkonly", false) ? " (key 40 only)" : ""));
        }
        // --es vivoset "1=1,55=74": any VivoCameraManager values, each restored in finally
        final java.util.LinkedHashMap<Integer, Integer> restore = new java.util.LinkedHashMap<>();
        final String vivoset = getIntent().getStringExtra("vivoset");
        if (vivoset != null && !vivoset.isEmpty()) {
            Provider vp = provider();
            for (String kv : vivoset.split(",")) {
                String[] parts = kv.split("=");
                int key = Integer.parseInt(parts[0].trim()), value = Integer.parseInt(parts[1].trim());
                if (!restore.containsKey(key)) restore.put(key, vp.get(key));
                vp.set(key, value);
            }
            SystemClock.sleep(100);
            StringBuilder now = new StringBuilder();
            for (int key : restore.keySet()) now.append(key).append('=').append(vp.get(key)).append(' ');
            meta.println("vivoset " + vivoset + " -> now " + now + " (was " + restore + ")");
        }
        HandlerThread cb = new HandlerThread("probe-stock");
        cb.start();
        final Handler h = new Handler(cb.getLooper());
        final Object lock = new Object();
        final CameraDevice[] device = new CameraDevice[1];
        final CameraCaptureSession[] session = new CameraCaptureSession[1];
        final String[] error = new String[1];
        final int[] previews = new int[1];
        final File[] saved = new File[2];
        // --ei pvw / pvh: YUV preview size; --ez pvyuvoff: the YUV is configured but not a repeating target
        final ImageReader preview = ImageReader.newInstance(getIntent().getIntExtra("pvw", 1440), getIntent().getIntExtra("pvh", 1080),
                ImageFormat.YUV_420_888, 4);
        final boolean previewYuvOff = getIntent().getBooleanExtra("pvyuvoff", false);
        // --el fullusage N: the full-size RAW reader's usage (the stock stream: 0x131075 combined; 0 = no CPU flags)
        final long fullUsage = getIntent().getLongExtra("fullusage", 0L);
        final int burst = getIntent().getIntExtra("burst", 1);  // --ei burst N: N back-to-back stills
        // --ez rawvf: the binned RAW stream is the viewfinder (repeating target); every binned timestamp is logged
        final boolean rawVf = getIntent().getBooleanExtra("rawvf", false);
        final boolean noYuv = getIntent().getBooleanExtra("noyuv", false);  // --ez noyuv: no full-size YUV stream
        // --ez rawonly: only the two RAW streams; the binned one is the viewfinder (as rawr streams)
        final boolean rawOnly = getIntent().getBooleanExtra("rawonly", false);
        final String exps = stringExtra("exps", "");  // --es exps a,b,..: per-still exposure (ns), cycled over the burst
        final java.util.List<Long> binnedFrames = java.util.Collections.synchronizedList(new java.util.ArrayList<>());
        final int[] fullIndex = {0};
        final java.util.List<Long> fullFrames = java.util.Collections.synchronizedList(new java.util.ArrayList<>());
        final long[] burstT0 = {0};
        final ImageReader rawFull = fullUsage == 3 ? ImageReader.newInstance(fw, fh, ImageFormat.RAW_SENSOR, 2)
                : ImageReader.newInstance(fw, fh, ImageFormat.RAW_SENSOR, 2, fullUsage);
        meta.println("full RAW reader usage " + fullUsage + (fullUsage == 3 ? " (default CPU_READ_OFTEN)" : ""));
        final ImageReader yuvFull = ImageReader.newInstance(fw, fh, ImageFormat.YUV_420_888, 1);
        final ImageReader rawBinned = ImageReader.newInstance(bw, bh, ImageFormat.RAW_SENSOR, 2,
                getIntent().getLongExtra("binnedusage", 0L));  // --el binnedusage
        try {
            preview.setOnImageAvailableListener(r -> { Image i = r.acquireNextImage(); if (i != null) { previews[0]++; i.close(); } }, h);
            yuvFull.setOnImageAvailableListener(r -> { Image i = r.acquireNextImage(); if (i != null) { meta.println("full YUV frame (unexpected)"); i.close(); } }, h);
            ImageReader.OnImageAvailableListener rawSaver = r -> {
                Image img = r.acquireNextImage();
                if (img == null) return;
                final int slot = r == rawFull ? 0 : 1;
                if (slot == 1 && (rawVf || rawOnly)) {
                    binnedFrames.add(img.getTimestamp());
                    img.close();
                    return;
                }
                if (slot == 0) fullFrames.add(img.getTimestamp());
                final int index = slot == 0 ? fullIndex[0]++ : 0;
                if (slot == 0 && burst > 1 && index == burst - 1) {
                    try {
                        Image.Plane plane = img.getPlanes()[0];
                        ByteBuffer buf = plane.getBuffer();
                        int stride = plane.getRowStride();
                        File out = new File(dir, "full_last_" + img.getWidth() + "x" + img.getHeight() + ".raw");
                        try (FileOutputStream o = new FileOutputStream(out)) {
                            byte[] line = new byte[img.getWidth() * 2];
                            for (int y = 0; y < img.getHeight(); y++) { buf.position(y * stride); buf.get(line); o.write(line); }
                        }
                        meta.println("raw " + out.getName() + " ts=" + img.getTimestamp());
                    } catch (Exception e) {
                        meta.println("raw write failed: " + e);
                    } finally {
                        img.close();
                        synchronized (lock) { lock.notifyAll(); }
                    }
                    return;
                }
                if (saved[slot] != null) {  // burst: keep only the first frame of each stream
                    meta.println("raw " + (slot == 0 ? "full" : "binned") + " frame ts=" + img.getTimestamp() + " at " + (SystemClock.elapsedRealtime() - burstT0[0]) + "ms");
                    img.close();
                    synchronized (lock) { lock.notifyAll(); }
                    return;
                }
                try {
                    Image.Plane plane = img.getPlanes()[0];
                    ByteBuffer buf = plane.getBuffer();
                    int stride = plane.getRowStride();
                    File out = new File(dir, (slot == 0 ? "full_" : "binned_") + img.getWidth() + "x" + img.getHeight() + ".raw");
                    try (FileOutputStream o = new FileOutputStream(out)) {
                        byte[] line = new byte[img.getWidth() * 2];
                        for (int y = 0; y < img.getHeight(); y++) { buf.position(y * stride); buf.get(line); o.write(line); }
                    }
                    meta.println("raw " + out.getName() + " stride=" + stride + " ts=" + img.getTimestamp());
                    saved[slot] = out;
                } catch (Exception e) {
                    meta.println("raw write failed: " + e);
                } finally {
                    img.close();
                    synchronized (lock) { lock.notifyAll(); }
                }
            };
            HandlerThread writer = new HandlerThread("probe-writer");
            writer.start();
            rawFull.setOnImageAvailableListener(rawSaver, new Handler(writer.getLooper()));
            rawBinned.setOnImageAvailableListener(rawSaver, h);

            CameraManager manager = getSystemService(CameraManager.class);
            manager.openCamera(cam, new CameraDevice.StateCallback() {
                @Override public void onOpened(CameraDevice d) { synchronized (lock) { device[0] = d; lock.notifyAll(); } }
                @Override public void onDisconnected(CameraDevice d) { error[0] = "disconnected"; d.close(); synchronized (lock) { lock.notifyAll(); } }
                @Override public void onError(CameraDevice d, int e) { error[0] = "device error " + e; d.close(); synchronized (lock) { lock.notifyAll(); } }
            }, h);
            synchronized (lock) { if (device[0] == null && error[0] == null) lock.wait(5000); }
            if (device[0] == null) throw new IllegalStateException("open failed: " + error[0]);

            CaptureRequest.Builder sb = device[0].createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW);
            if (sessionDrop.isEmpty()) stockSessionKeys(sb); else sessionKeys(sb);
            SessionConfiguration config = new SessionConfiguration(SessionConfiguration.SESSION_REGULAR,
                rawOnly ? java.util.Arrays.asList(new OutputConfiguration(rawFull.getSurface()), new OutputConfiguration(rawBinned.getSurface()))
                : noYuv ? java.util.Arrays.asList(new OutputConfiguration(preview.getSurface()), new OutputConfiguration(rawFull.getSurface()),
                            new OutputConfiguration(rawBinned.getSurface()))
                      : java.util.Arrays.asList(new OutputConfiguration(preview.getSurface()), new OutputConfiguration(rawFull.getSurface()),
                            new OutputConfiguration(yuvFull.getSurface()), new OutputConfiguration(rawBinned.getSurface())),
                    h::post, new CameraCaptureSession.StateCallback() {
                        @Override public void onConfigured(CameraCaptureSession s) { synchronized (lock) { session[0] = s; lock.notifyAll(); } }
                        @Override public void onConfigureFailed(CameraCaptureSession s) { error[0] = "configure failed"; synchronized (lock) { lock.notifyAll(); } }
                    });
            config.setSessionParameters(sb.build());
            device[0].createCaptureSession(config);
            synchronized (lock) { if (session[0] == null && error[0] == null) lock.wait(8000); }
            if (session[0] == null) throw new IllegalStateException("session failed: " + error[0]);
            meta.println("session configured");

            CaptureRequest.Builder pb = device[0].createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW);
            if (!rawOnly && !previewYuvOff) pb.addTarget(preview.getSurface());
            // --es rawrpv a,b: rawr's preview statistics settings (shading, hotmap, faces, nodist, nohot)
            for (String opt : stringExtra("rawrpv", "").split(",")) {
                switch (opt) {
                    case "shading": pb.set(CaptureRequest.STATISTICS_LENS_SHADING_MAP_MODE, CaptureRequest.STATISTICS_LENS_SHADING_MAP_MODE_ON); break;
                    case "hotmap": pb.set(CaptureRequest.STATISTICS_HOT_PIXEL_MAP_MODE, true); break;
                    case "faces": pb.set(CaptureRequest.STATISTICS_FACE_DETECT_MODE, CaptureRequest.STATISTICS_FACE_DETECT_MODE_SIMPLE); break;
                    case "nodist": pb.set(CaptureRequest.DISTORTION_CORRECTION_MODE, CaptureRequest.DISTORTION_CORRECTION_MODE_OFF); break;
                    case "nohot": pb.set(CaptureRequest.HOT_PIXEL_MODE, CaptureRequest.HOT_PIXEL_MODE_OFF); break;
                    case "aeoff":
                        pb.set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_OFF);
                        pb.set(CaptureRequest.SENSOR_EXPOSURE_TIME, 33_000_000L);
                        pb.set(CaptureRequest.SENSOR_SENSITIVITY, 200);
                        pb.set(CaptureRequest.SENSOR_FRAME_DURATION, 33_333_333L);
                        break;
                    case "aeoffdim":  // rawr AE in a dim room: 1/15 s at ISO 800
                        pb.set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_OFF);
                        pb.set(CaptureRequest.SENSOR_EXPOSURE_TIME, 66_666_667L);
                        pb.set(CaptureRequest.SENSOR_SENSITIVITY, 800);
                        pb.set(CaptureRequest.SENSOR_FRAME_DURATION, 66_666_667L);
                        break;
                    case "cchq": pb.set(CaptureRequest.COLOR_CORRECTION_MODE, CaptureRequest.COLOR_CORRECTION_MODE_HIGH_QUALITY); break;
                    case "prio": pb.set(new CaptureRequest.Key<>("android.control.aePriorityMode", int.class), 0); break;
                    case "fps": pb.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, new android.util.Range<>(12, 30)); break;
                    default: break;
                }
            }
            if (rawVf || rawOnly) pb.addTarget(rawBinned.getSurface());
            meta.println("rawvf=" + rawVf + " noyuv=" + noYuv + " burst=" + burst + " exps=" + exps);
            // --ef focus D: lens fixed at D diopters on the preview and the still (no AF)
            final float fixedFocus = getIntent().getFloatExtra("focus", -1f);
            if (fixedFocus >= 0) {
                pb.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_OFF);
                pb.set(CaptureRequest.LENS_FOCUS_DISTANCE, fixedFocus);
                meta.println("focus fixed at " + fixedFocus + " dpt");
            } else {
                pb.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_PICTURE);
            }
            // --ef afx / afy: AF (and AE) region centre as a fraction of the sensor frame (landscape, sensor orientation)
            final float afx = getIntent().getFloatExtra("afx", -1f), afy = getIntent().getFloatExtra("afy", -1f);
            if (afx >= 0 && afy >= 0) {
                android.graphics.Rect active = manager.getCameraCharacteristics(cam).get(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE);
                int rw = active.width() / 10, rh = active.height() / 10;
                int cx = (int) (afx * active.width()), cy = (int) (afy * active.height());
                android.hardware.camera2.params.MeteringRectangle[] region = {new android.hardware.camera2.params.MeteringRectangle(
                        Math.max(0, cx - rw / 2), Math.max(0, cy - rh / 2), rw, rh, android.hardware.camera2.params.MeteringRectangle.METERING_WEIGHT_MAX)};
                pb.set(CaptureRequest.CONTROL_AF_REGIONS, region);
                meta.println("AF region " + region[0]);
            }
            if (!stockMinKeys && stockDrop.isEmpty()) setInt(pb, "vivo.control.ultra_highresolution", stockUltra);
            if (getIntent().getBooleanExtra("allkeys", true)) {  // the stock app repeats them on every request
                if (previewDrop.isEmpty()) stockSessionKeys(pb); else previewKeys(pb);
            }
            final Float[] focus = {null};
            final int[] lost = {0};
            session[0].setRepeatingRequest(pb.build(), new CameraCaptureSession.CaptureCallback() {
                @Override public void onCaptureCompleted(CameraCaptureSession s, CaptureRequest r, TotalCaptureResult res) {
                    focus[0] = res.get(CaptureResult.LENS_FOCUS_DISTANCE);
                }
                @Override public void onCaptureBufferLost(CameraCaptureSession s, CaptureRequest r, android.view.Surface t, long f) {
                    if (lost[0]++ < 3) meta.println("preview buffer lost frame " + f + (t == rawBinned.getSurface() ? " (binned RAW)" : ""));
                }
                @Override public void onCaptureFailed(CameraCaptureSession s, CaptureRequest r, CaptureFailure f) {
                    if (lost[0]++ < 3) meta.println("preview capture failed reason=" + f.getReason());
                }
            }, h);
            SystemClock.sleep(3000);
            // --ei aftaps N: N tap-to-focus sequences 2 s apart (vivo camera's: CANCEL in continuous AF without
            // regions, then START in AUTO with the region as AF and AE region), at random points
            final int afTaps = getIntent().getIntExtra("aftaps", 0);
            if (afTaps > 0) {
                android.graphics.Rect active = manager.getCameraCharacteristics(cam).get(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE);
                java.util.Random random = new java.util.Random(7);
                for (int t = 0; t < afTaps && error[0] == null; ++t) {
                    CaptureRequest.Builder cancel = device[0].createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW);
                    CaptureRequest.Builder start = device[0].createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW);
                    for (CaptureRequest.Builder b : new CaptureRequest.Builder[]{cancel, start}) {
                        if (!rawOnly && !previewYuvOff) b.addTarget(preview.getSurface());
                        if (rawVf || rawOnly) b.addTarget(rawBinned.getSurface());
                        if (previewDrop.isEmpty()) stockSessionKeys(b); else previewKeys(b);
                    }
                    for (CaptureRequest.Builder b : new CaptureRequest.Builder[]{cancel, start})
                        for (CaptureRequest.Key<?> k : new CaptureRequest.Key<?>[]{CaptureRequest.CONTROL_AE_MODE,
                                CaptureRequest.SENSOR_EXPOSURE_TIME, CaptureRequest.SENSOR_SENSITIVITY,
                                CaptureRequest.SENSOR_FRAME_DURATION, CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE,
                                CaptureRequest.STATISTICS_LENS_SHADING_MAP_MODE, CaptureRequest.STATISTICS_HOT_PIXEL_MAP_MODE,
                                CaptureRequest.STATISTICS_FACE_DETECT_MODE, CaptureRequest.DISTORTION_CORRECTION_MODE,
                                CaptureRequest.HOT_PIXEL_MODE, CaptureRequest.COLOR_CORRECTION_MODE})
                            copyKey(pb, b, k);
                    cancel.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_PICTURE);
                    cancel.set(CaptureRequest.CONTROL_AF_TRIGGER, CaptureRequest.CONTROL_AF_TRIGGER_CANCEL);
                    int w = active.width() * 3 / 20, x = random.nextInt(active.width() - w), y = random.nextInt(active.height() - w);
                    android.hardware.camera2.params.MeteringRectangle[] region = {
                            new android.hardware.camera2.params.MeteringRectangle(x, y, w, w, 250)};
                    start.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_AUTO);
                    start.set(CaptureRequest.CONTROL_AF_REGIONS, region);
                    start.set(CaptureRequest.CONTROL_AE_REGIONS, region);
                    start.set(CaptureRequest.CONTROL_AF_TRIGGER, CaptureRequest.CONTROL_AF_TRIGGER_START);
                    session[0].captureBurst(java.util.Arrays.asList(cancel.build(), start.build()), null, h);
                    // after the start the repeating request holds AUTO + region at IDLE, like vivo's camera
                    pb.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_AUTO);
                    pb.set(CaptureRequest.CONTROL_AF_REGIONS, region);
                    pb.set(CaptureRequest.CONTROL_AE_REGIONS, region);
                    session[0].setRepeatingRequest(pb.build(), null, h);
                    if (getIntent().getBooleanExtra("churn", false)) {
                        // rawr AE: a new repeating request about every frame with a slightly different exposure
                        Long base = pb.get(CaptureRequest.SENSOR_EXPOSURE_TIME);
                        for (int k = 0; k < 28 && base != null; ++k) {
                            pb.set(CaptureRequest.SENSOR_EXPOSURE_TIME, (long) (base * (0.95 + 0.1 * random.nextDouble())));
                            session[0].setRepeatingRequest(pb.build(), null, h);
                            SystemClock.sleep(70);
                        }
                        pb.set(CaptureRequest.SENSOR_EXPOSURE_TIME, base);
                    } else {
                        SystemClock.sleep(2000);
                    }
                }
                meta.println("af taps " + afTaps + " done error=" + error[0]);
            }
            meta.println("preview frames before the still: " + previews[0] + " focus=" + focus[0]);

            CaptureRequest.Builder still = device[0].createCaptureRequest(CameraDevice.TEMPLATE_STILL_CAPTURE);
            still.addTarget(rawBinned.getSurface());
            still.addTarget(rawFull.getSurface());
            still.set(CaptureRequest.CONTROL_CAPTURE_INTENT, CaptureRequest.CONTROL_CAPTURE_INTENT_STILL_CAPTURE);
            still.set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_OFF);
            still.set(CaptureRequest.SENSOR_SENSITIVITY, iso);
            still.set(CaptureRequest.SENSOR_EXPOSURE_TIME, exp);
            still.set(CaptureRequest.SENSOR_FRAME_DURATION, exp);
            still.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_OFF);
            if (fixedFocus >= 0) still.set(CaptureRequest.LENS_FOCUS_DISTANCE, fixedFocus);
            else if (focus[0] != null) still.set(CaptureRequest.LENS_FOCUS_DISTANCE, focus[0]);
            if (!stockMinKeys && stockDrop.isEmpty()) setInt(still, "vivo.control.ultra_highresolution", stockUltra);
            if (getIntent().getBooleanExtra("allkeys", true)) stockSessionKeys(still);
            // the stock app's still keys (cmd media.camera watch, 2026-10-17): a DNG-only seamless 200 MP capture
            if (getIntent().getBooleanExtra("stillkeys", true)) {
                long id = System.currentTimeMillis();
                setInt(still, "vivo.control.onlyDng", 1);
                setInt(still, "vivo.control.isCapture", 0);
                setInt(still, "vivo.control.sensorMode", 0);
                setInt(still, "vivo.control.isUpscale", stockUltra);
                still.set(new CaptureRequest.Key<>("vivo.control.requestId", long[].class), new long[]{id});
                still.set(new CaptureRequest.Key<>("vivo.control.globalCaptureId", long[].class), new long[]{id + 5});
                still.set(new CaptureRequest.Key<>("vivo.control.frameFlag", long[].class), new long[]{id});
                still.set(new CaptureRequest.Key<>("vivo.control.isPAPNeeded", byte[].class), new byte[]{1});
                meta.println("still keys: onlyDng 1, isCapture 0, sensorMode 0, isUpscale " + stockUltra + ", isPAPNeeded 1, ids " + id);
            }
            final long t0 = SystemClock.elapsedRealtime();
            burstT0[0] = t0;
            java.util.List<CaptureRequest> stills = new java.util.ArrayList<>();
            for (int i = 0; i < burst; ++i) {
                long id = System.currentTimeMillis() + i;
                still.set(new CaptureRequest.Key<>("vivo.control.requestId", long[].class), new long[]{id});
                still.set(new CaptureRequest.Key<>("vivo.control.globalCaptureId", long[].class), new long[]{id + 5});
                still.set(new CaptureRequest.Key<>("vivo.control.frameFlag", long[].class), new long[]{id});
                final int stillMode = getIntent().getIntExtra("stillmode", -1);  // --ei stillmode N: forceSensorMode on stills only
                if (stillMode >= 0) setInt(still, "vivo.control.forceSensorMode", stillMode);
                if (!exps.isEmpty()) {
                    String[] e = exps.split(",");
                    long ns = Long.parseLong(e[i % e.length]);
                    still.set(CaptureRequest.SENSOR_EXPOSURE_TIME, ns);
                    still.set(CaptureRequest.SENSOR_FRAME_DURATION, Math.max(ns, 33_333_333L));
                }
                stills.add(still.build());
            }
            session[0].captureBurst(stills, new CameraCaptureSession.CaptureCallback() {
                @Override public void onCaptureCompleted(CameraCaptureSession s, CaptureRequest r, TotalCaptureResult res) {
                    meta.println("still result at " + (SystemClock.elapsedRealtime() - t0) + "ms exp=" + res.get(CaptureResult.SENSOR_EXPOSURE_TIME)
                            + " iso=" + res.get(CaptureResult.SENSOR_SENSITIVITY) + " focus=" + res.get(CaptureResult.LENS_FOCUS_DISTANCE)
                            + " seamlessMode=" + vendor(res, "vcf.parameter.seamlessMode") + " dngType=" + vendor(res, "vivo.parameter.highResolutionDngType")
                            + " remosaic=" + vendor(res, "vivo.control.seamless.remosaic.enable"));
                }
                @Override public void onCaptureFailed(CameraCaptureSession s, CaptureRequest r, CaptureFailure f) {
                    meta.println("still failed reason=" + f.getReason());
                    synchronized (lock) { lock.notifyAll(); }
                }
            }, h);
            final long deadline = SystemClock.elapsedRealtime() + 30000;
            synchronized (lock) {
                while ((saved[0] == null || (saved[1] == null && !rawVf && !rawOnly) || fullFrames.size() < burst) && error[0] == null
                        && SystemClock.elapsedRealtime() < deadline)
                    lock.wait(1000);
            }
            meta.println("full frames " + fullFrames.size() + "/" + burst + " ts=" + fullFrames);
            if (rawVf || rawOnly) {
                SystemClock.sleep(500);  // viewfinder frames after the burst
                java.util.List<Long> b = new java.util.ArrayList<>(binnedFrames);
                StringBuilder gaps = new StringBuilder();
                for (int i = 1; i < b.size(); i++) gaps.append((b.get(i) - b.get(i - 1)) / 100_000 / 10.0).append(fullFrames.contains(b.get(i)) ? "*" : "").append(' ');
                meta.println("viewfinder frames " + b.size() + " gaps ms (* = burst frame): " + gaps);
            }
            meta.println("full=" + saved[0] + " binned=" + saved[1] + " previews total=" + previews[0] + " error=" + error[0]
                    + " took=" + (SystemClock.elapsedRealtime() - t0) + "ms");
            try { session[0].stopRepeating(); } catch (Exception ignored) { }
        } catch (Exception e) {
            meta.println("EXCEPTION " + e);
            throw e;
        } finally {
            if (device[0] != null) device[0].close();
            SystemClock.sleep(300);
            preview.close(); rawFull.close(); yuvFull.close(); rawBinned.close();
            cb.quitSafely();
            if (p != null) {
                p.set(VCF_KEY, oldVcf);
                p.set(SDK_KEY, oldSdk);
                SystemClock.sleep(100);
                meta.println("lease restored=" + p.get(SDK_KEY) + "/" + p.get(VCF_KEY));
            }
            if (!restore.isEmpty()) {
                Provider vp = provider();
                java.util.List<Integer> keys = new java.util.ArrayList<>(restore.keySet());
                Collections.reverse(keys);
                for (int key : keys) vp.set(key, restore.get(key));
                SystemClock.sleep(100);
                StringBuilder now = new StringBuilder();
                for (int key : restore.keySet()) now.append(key).append('=').append(vp.get(key)).append(' ');
                meta.println("vivoset restored " + now);
            }
            meta.println("END");
            meta.close();
        }
    }

    // ---- vivo SDK lease ------------------------------------------------------------------------------------------------

    interface Provider { int get(int key) throws Exception; void set(int key, int value) throws Exception; }

    private static Provider provider() throws Exception {
        Class<?> api = Class.forName("android.hardware.vivocamera.VivoCameraManager");
        Object manager = api.getMethod("getInstance").invoke(null);
        Method get = api.getMethod("getInfo", int.class), set = api.getMethod("setValue", int.class, int.class);
        return new Provider() {
            @Override public int get(int key) throws Exception { return (Integer) get.invoke(manager, key); }
            @Override public void set(int key, int value) throws Exception { set.invoke(manager, key, value); }
        };
    }

    // ---- capture --------------------------------------------------------------------------------------------------------

    private void capture() throws Exception {
        final String cam = stringExtra("cam", "5");
        final int row = getIntent().getIntExtra("row", -1);
        final int w = getIntent().getIntExtra("w", 0), h = getIntent().getIntExtra("h", 0);
        final int frames = getIntent().getIntExtra("frames", 3);
        final boolean lease = getIntent().getBooleanExtra("lease", true);
        final String run = String.format(Locale.US, "cam%s_row%d_%dx%d%s", cam, row, w, h, lease ? "" : "_nolease");
        final File dir = runDir(run);
        for (File f : dir.listFiles()) f.delete();
        final PrintWriter meta = new PrintWriter(new File(dir, "meta.txt"));
        meta.println("cam=" + cam + " row=" + row + " size=" + w + "x" + h + " lease=" + lease + " pkg=" + getPackageName());

        Provider p = null;
        int oldSdk = 0, oldVcf = 0;
        if (lease) {
            p = provider();
            oldSdk = p.get(SDK_KEY);
            oldVcf = p.get(VCF_KEY);
            // --ez sdkonly true: only key 40 (useVivoCamera); cameraserver sets 33 itself at open
            if (!getIntent().getBooleanExtra("sdkonly", false)) p.set(VCF_KEY, 2);
            p.set(SDK_KEY, 1);
            SystemClock.sleep(100);
            meta.println("lease before=" + oldSdk + "/" + oldVcf + " after=" + p.get(SDK_KEY) + "/" + p.get(VCF_KEY)
                    + (getIntent().getBooleanExtra("sdkonly", false) ? " (key 40 only)" : ""));
        }
        final Object lock = new Object();
        final CameraDevice[] device = new CameraDevice[1];
        final String[] error = new String[1];
        final int[] got = new int[1];
        final int[] seen = new int[1];
        final int still = getIntent().getIntExtra("still", 0);
        final int warmup = getIntent().getIntExtra("warmup", 4);
        // --ez af true: continuous AF on the requests; --ef focus D: AF off, lens at D diopters (from an AF run)
        final boolean af = getIntent().getBooleanExtra("af", false);
        final float focus = getIntent().getFloatExtra("focus", -1f);
        final float[] lastFocus = {-1f};
        final int[] lastAf = {-1};
        if (still > 0) meta.println("still route " + still + " (YUV 1920x1440 preview + one RAW capture)");
        final ImageReader preview = still > 0 ? ImageReader.newInstance(1920, 1440, ImageFormat.YUV_420_888, 4) : null;
        final int[] previews = new int[1];
        // --el usage N: the RAW reader's AHB usage (default 0; rawr's reader asks for CPU and/or GPU bits)
        final long readerUsage = getIntent().getLongExtra("usage", 0L);
        meta.println("reader usage 0x" + Long.toHexString(readerUsage));
        ImageReader reader = ImageReader.newInstance(w, h, ImageFormat.RAW_SENSOR, (long) w * h > 100_000_000L ? 2 : 4, readerUsage);
        // camera callbacks on their own thread: this one blocks waiting for them
        HandlerThread callbacks = new HandlerThread("probe-cb");
        callbacks.start();
        final Handler handler = new Handler(callbacks.getLooper());
        try {
            CameraManager manager = getSystemService(CameraManager.class);
            final long t0 = SystemClock.elapsedRealtime();
            reader.setOnImageAvailableListener(r -> {
                Image img = r.acquireNextImage();
                if (img == null) return;
                try {
                    seen[0]++;
                    // skip the first frames while AE settles
                    if (still == 0 && seen[0] <= warmup || got[0] >= frames) return;
                    Image.Plane plane = img.getPlanes()[0];
                    ByteBuffer buf = plane.getBuffer();
                    int stride = plane.getRowStride();
                    String name = "frame" + got[0] + ".raw";
                    try (FileOutputStream out = new FileOutputStream(new File(dir, name))) {
                        byte[] line = new byte[img.getWidth() * 2];
                        for (int y = 0; y < img.getHeight(); y++) {
                            buf.position(y * stride);
                            buf.get(line);
                            out.write(line);
                        }
                    }
                    String s = "image " + got[0] + " " + img.getWidth() + "x" + img.getHeight() + " stride=" + stride
                            + " pixelStride=" + plane.getPixelStride() + " bytes=" + buf.capacity() + " ts=" + img.getTimestamp()
                            + " at=" + (SystemClock.elapsedRealtime() - t0) + "ms";
                    meta.println(s);
                    Log.i(TAG, s);
                    got[0]++;
                    if (got[0] >= frames) synchronized (lock) { lock.notifyAll(); }
                } catch (Exception e) {
                    error[0] = "write: " + e;
                } finally {
                    img.close();
                }
            }, handler);

            if (preview != null) preview.setOnImageAvailableListener(r -> {
                Image img = r.acquireNextImage();
                if (img != null) { previews[0]++; img.close(); }
            }, handler);
            manager.openCamera(cam, new CameraDevice.StateCallback() {
                @Override public void onOpened(CameraDevice d) { synchronized (lock) { device[0] = d; lock.notifyAll(); } }
                @Override public void onDisconnected(CameraDevice d) { error[0] = "disconnected"; d.close(); synchronized (lock) { lock.notifyAll(); } }
                @Override public void onError(CameraDevice d, int e) { error[0] = "device error " + e; d.close(); synchronized (lock) { lock.notifyAll(); } }
            }, handler);
            synchronized (lock) { if (device[0] == null && error[0] == null) lock.wait(5000); }
            if (device[0] == null) throw new IllegalStateException("open failed: " + error[0]);
            // --es early open|configured: give key 40 back right after the device opens / the session is
            // configured, to test whether cameraserver reads it only then
            if ("open".equals(getIntent().getStringExtra("early")) && p != null) {
                p.set(SDK_KEY, oldSdk);
                SystemClock.sleep(100);
                meta.println("key 40 restored early (after open): now " + p.get(SDK_KEY));
            }

            CaptureRequest.Builder b = device[0].createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW);
            b.addTarget(reader.getSurface());
            b.set(CaptureRequest.CONTROL_MODE, CaptureRequest.CONTROL_MODE_AUTO);
            // manual exposure at the sensor's lowest ISO unless --ez auto true
            if (getIntent().getBooleanExtra("auto", false)) {
                b.set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_ON);
            } else {
                android.util.Range<Integer> iso = manager.getCameraCharacteristics(cam)
                        .get(CameraCharacteristics.SENSOR_INFO_SENSITIVITY_RANGE);
                long exp = getIntent().getLongExtra("exp", 33_000_000L);
                b.set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_OFF);
                int isoValue = getIntent().getIntExtra("iso", iso.getLower());  // --ei iso N, default the lowest
                b.set(CaptureRequest.SENSOR_SENSITIVITY, isoValue);
                b.set(CaptureRequest.SENSOR_EXPOSURE_TIME, exp);
                b.set(CaptureRequest.SENSOR_FRAME_DURATION, exp);
                meta.println("manual iso=" + isoValue + " exp=" + exp);
            }
            if (row >= 0 && row < 64) b.set(new CaptureRequest.Key<>("vivo.control.forceSensorMode", int[].class), new int[]{row});
            if (af) {
                b.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_PICTURE);
            } else if (focus >= 0) {
                b.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_OFF);
                b.set(CaptureRequest.LENS_FOCUS_DISTANCE, focus);
                meta.println("focus fixed at " + focus + " dpt");
            }
            if (getIntent().getBooleanExtra("vivokeys", false)) vivoKeys(b, meta);
            extraKeys(b, meta);
            final CaptureRequest request = b.build();

            Executor exec = handler::post;
            final CameraCaptureSession[] session = new CameraCaptureSession[1];
            SessionConfiguration config = new SessionConfiguration(SessionConfiguration.SESSION_REGULAR,
                    still > 0 ? java.util.Arrays.asList(new OutputConfiguration(preview.getSurface()), new OutputConfiguration(reader.getSurface()))
                            : Collections.singletonList(new OutputConfiguration(reader.getSurface())), exec,
                    new CameraCaptureSession.StateCallback() {
                        @Override public void onConfigured(CameraCaptureSession s) { synchronized (lock) { session[0] = s; lock.notifyAll(); } }
                        @Override public void onConfigureFailed(CameraCaptureSession s) { error[0] = "configure failed"; synchronized (lock) { lock.notifyAll(); } }
                    });
            // minimal session parameters: forced mode, auto AE, the auto FPS range only
            CaptureRequest.Builder sb = device[0].createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW);
            sb.set(CaptureRequest.CONTROL_MODE, CaptureRequest.CONTROL_MODE_AUTO);
            sb.set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_ON);
            sb.set(CaptureRequest.CONTROL_AE_LOCK, false);
            if (row >= 0 && row < 64 && still != 2) sb.set(new CaptureRequest.Key<>("vivo.control.forceSensorMode", int[].class), new int[]{row});
            sb.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, new android.util.Range<>(minFps(manager, cam), 30));
            final boolean vivo = getIntent().getBooleanExtra("vivokeys", false);
            if (vivo) vivoKeys(sb, meta);
            extraKeys(sb, meta);
            if (getIntent().getStringExtra("keys") != null) meta.println("extra keys " + getIntent().getStringExtra("keys"));
            config.setSessionParameters(sb.build());
            device[0].createCaptureSession(config);
            synchronized (lock) { if (session[0] == null && error[0] == null) lock.wait(8000); }
            if (session[0] == null) throw new IllegalStateException("session failed: " + error[0]);
            // --es early open|configured: give key 40 back right after the device opens / the session is
            // configured, to test whether cameraserver reads it only then
            if ("configured".equals(getIntent().getStringExtra("early")) && p != null) {
                p.set(SDK_KEY, oldSdk);
                SystemClock.sleep(100);
                meta.println("key 40 restored early (after session configured): now " + p.get(SDK_KEY));
            }

            final int[] results = new int[1];
            final CameraCaptureSession.CaptureCallback callback = new CameraCaptureSession.CaptureCallback() {
                @Override public void onCaptureCompleted(CameraCaptureSession s, CaptureRequest r, TotalCaptureResult result) {
                    Float fd = result.get(CaptureResult.LENS_FOCUS_DISTANCE);
                    Integer as = result.get(CaptureResult.CONTROL_AF_STATE);
                    if (fd != null) lastFocus[0] = fd;
                    if (as != null) lastAf[0] = as;
                    if (results[0]++ >= 8) return;
                    meta.println("result " + result.getFrameNumber() + " exp=" + result.get(CaptureResult.SENSOR_EXPOSURE_TIME)
                            + " iso=" + result.get(CaptureResult.SENSOR_SENSITIVITY)
                            + " black=" + format(result.get(CaptureResult.SENSOR_DYNAMIC_BLACK_LEVEL))
                            + " white=" + result.get(CaptureResult.SENSOR_DYNAMIC_WHITE_LEVEL)
                            + " forced=" + vendor(result, "vivo.control.forceSensorMode")
                            + " sensorMode=" + vendor(result, "vivo.feedback.sensorMode")
                            + " crop=" + result.get(CaptureResult.SCALER_CROP_REGION));
                }
                @Override public void onCaptureFailed(CameraCaptureSession s, CaptureRequest r, CaptureFailure f) {
                    meta.println("capture failed reason=" + f.getReason() + " frame=" + f.getFrameNumber());
                }
            };
            if (still > 0) {
                CaptureRequest.Builder pb = device[0].createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW);
                pb.addTarget(preview.getSurface());
                pb.set(CaptureRequest.CONTROL_MODE, CaptureRequest.CONTROL_MODE_AUTO);
                pb.set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_ON);
                if (row >= 0 && row < 64 && still == 1)
                    pb.set(new CaptureRequest.Key<>("vivo.control.forceSensorMode", int[].class), new int[]{row});
                session[0].setRepeatingRequest(pb.build(), null, handler);
                SystemClock.sleep(2000);
                meta.println("preview frames before the still: " + previews[0]);
                session[0].capture(request, callback, handler);
            } else {
                session[0].setRepeatingRequest(request, callback, handler);
            }
            synchronized (lock) { if (got[0] < frames && error[0] == null) lock.wait(20000); }
            if (still > 0) meta.println("preview frames total: " + previews[0]);
            meta.println("last focus=" + lastFocus[0] + " dpt afState=" + lastAf[0]);
            meta.println("frames=" + got[0] + "/" + frames + " seen=" + seen[0] + " results=" + results[0] + " error=" + error[0]);
            Log.i(TAG, run + ": frames=" + got[0] + "/" + frames + " seen=" + seen[0] + " results=" + results[0] + " error=" + error[0]);
            try { session[0].stopRepeating(); } catch (Exception ignored) { }
        } catch (Exception e) {
            meta.println("EXCEPTION " + e);
            throw e;
        } finally {
            if (device[0] != null) device[0].close();
            SystemClock.sleep(300);
            reader.close();
            if (preview != null) preview.close();
            callbacks.quitSafely();
            if (p != null) {
                p.set(VCF_KEY, oldVcf);
                p.set(SDK_KEY, oldSdk);
                SystemClock.sleep(100);
                meta.println("lease restored=" + p.get(SDK_KEY) + "/" + p.get(VCF_KEY));
            }
            meta.println("END");
            meta.close();
        }
    }

    /** vivo stock pro-mode "fullsize" request keys; unknown tags are logged into meta and skipped. */
    private static void vivoKeys(CaptureRequest.Builder b, PrintWriter meta) {
        Object[][] keys = {
                {"com.vivo.current_ui_module", new int[]{16}}, {"vivo.control.currentModeEx", new long[]{256}},
                {"vivo.control.is_pro_mode", new int[]{1}}, {"vivo.control.echo.mode", new int[]{1}},
                {"vivo.control.eis.config.enable", new int[]{0}}, {"vivo.control.advance_fullsize", new int[]{1}},
                {"vivo.control.ai_highresolution", new int[]{1}}, {"vivo.control.reprocessMode", new int[]{0}},
                {"vivo.control.streamsUsage", new int[]{7}}};  // stream role of a RAW stream
        for (Object[] k : keys) {
            try {
                if (k[1] instanceof int[]) b.set(new CaptureRequest.Key<>((String) k[0], int[].class), (int[]) k[1]);
                else b.set(new CaptureRequest.Key<>((String) k[0], long[].class), (long[]) k[1]);
            } catch (Exception e) {
                meta.println("key " + k[0] + " rejected: " + e);
            }
        }
    }

    /** --es keys "vivo.control.a=1,vivo.control.b=2": int32 vendor keys on the session and every request. */
    private void extraKeys(CaptureRequest.Builder b, PrintWriter meta) {
        String spec = getIntent().getStringExtra("keys");
        if (spec == null || spec.isEmpty()) return;
        for (String kv : spec.split(",")) {
            String[] p = kv.split("=");
            try {
                b.set(new CaptureRequest.Key<>(p[0].trim(), int[].class), new int[]{Integer.parseInt(p[1].trim())});
            } catch (Exception e) {
                meta.println("key " + kv + " rejected: " + e);
            }
        }
    }

    private static int minFps(CameraManager manager, String cam) throws Exception {
        android.util.Range<Integer>[] ranges = manager.getCameraCharacteristics(cam)
                .get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES);
        int min = 14;
        if (ranges != null) for (android.util.Range<Integer> r : ranges) min = Math.min(min, r.getLower());
        return min;
    }

    private static String vendor(CaptureResult result, String name) {
        try {
            return format(result.get(new CaptureResult.Key<>(name, int[].class)));
        } catch (Exception e) {
            return "n/a";
        }
    }
}
