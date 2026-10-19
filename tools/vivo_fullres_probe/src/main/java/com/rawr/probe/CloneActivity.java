package com.rawr.probe;

import android.app.Activity;
import android.graphics.ImageFormat;
import android.hardware.HardwareBuffer;
import android.hardware.camera2.CameraCaptureSession;
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
import android.view.Surface;

import java.io.File;
import java.io.FileOutputStream;
import java.io.PrintWriter;
import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.List;

/**
 * vivo's own Photo-mode session (dumpsys of com.android.camera, ui module 1, 2026-10-19), cloned on logical camera 2,
 * plus RAW_SENSOR stream(s) outside the vivo.control.streamsUsage list, the way vivo's Pro mode adds its RAW:
 * <pre>
 * am start -n &lt;pkg&gt;/com.rawr.probe.CloneActivity [--es cam 2] [--ei raws 1] [--ei rw 4096 --ei rh 3072]
 *     [--ez rawvf false] [--ez still true] [--ei hold 6000] [--ez photokeys true]
 * </pre>
 * Streams (vivo's order and roles [5, 0,1,4,19,5]): YUV 1440x1080, PRIVATE 1440x1080, YUV 480x360, YUV 1920x1440,
 * YUV 4096x3072. raws = 0 checks the clone alone. Key 40 is held around createCaptureSession only and always restored.
 * Output: files/clone/meta.txt (END last) and raw&lt;stream&gt;_&lt;n&gt;.raw (uint16, stride removed).
 */
public final class CloneActivity extends Activity {
    private static final String TAG = "RAWRCLONE";
    private static final int SDK_KEY = 40, VCF_KEY = 33;

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        HandlerThread t = new HandlerThread("clone");
        t.start();
        new Handler(t.getLooper()).post(() -> {
            try { run(); } catch (Throwable e) { Log.e(TAG, "clone failed", e); }
            runOnUiThread(this::finish);
            t.quitSafely();
        });
    }

    private static void setInt(CaptureRequest.Builder b, String name, int... v) {
        try { b.set(new CaptureRequest.Key<>(name, int[].class), v); } catch (Exception ignored) { }
    }

    private static void setFloat(CaptureRequest.Builder b, String name, float... v) {
        try { b.set(new CaptureRequest.Key<>(name, float[].class), v); } catch (Exception ignored) { }
    }

    /** The non-default vendor values of vivo's Photo-mode request. */
    /** --es usage "2,0,5": vivo.control.streamsUsage matching the configured streams ([count, role...]). */
    private static int[] usage = {5, 0, 1, 4, 19, 5};

    /** --es ikeys "a.b=1,c.d=2" (int32) and --es bkeys "e.f=1" (byte): extra keys on the session and every request. */
    private static java.util.Map<String, Integer> intKeys = new java.util.LinkedHashMap<>(), byteKeys = new java.util.LinkedHashMap<>();

    private static void extraKeys(CaptureRequest.Builder b) {
        for (java.util.Map.Entry<String, Integer> e : intKeys.entrySet()) setInt(b, e.getKey(), e.getValue());
        for (java.util.Map.Entry<String, Integer> e : byteKeys.entrySet()) {
            try { b.set(new CaptureRequest.Key<>(e.getKey(), byte[].class), new byte[]{(byte) (int) e.getValue()}); } catch (Exception ignored) { }
        }
    }

    private static void parseKeys(String spec, java.util.Map<String, Integer> into) {
        if (spec == null) return;
        for (String kv : spec.split(",")) {
            int eq = kv.indexOf('=');
            if (eq > 0) into.put(kv.substring(0, eq).trim(), Integer.parseInt(kv.substring(eq + 1).trim()));
        }
    }

    /** --ez video true: vivo's Video-mode session (main 35 mm, 1080p30, DCG-HDR on; dumpsys 2026-10-19) instead of Photo. */
    private static boolean video = false;

    private static void photoKeys(CaptureRequest.Builder b) {
        if (video) videoDefaults(b); else photoDefaults(b);
        extraKeys(b);
    }

    private static void videoDefaults(CaptureRequest.Builder b) {
        setInt(b, "com.vivo.current_ui_module", 3);
        setInt(b, "vivo.control.streamsUsage", usage);
        setInt(b, "vivo.control.videoMode", 1080);
        setInt(b, "vivo.control.videoFrameRate", 30);
        setInt(b, "vivo.control.eis.config.enable", 14);
        setInt(b, "vivo.control.EISsolution", 2);
        setInt(b, "vivo.control.uiSwitch_state", 16);
        setInt(b, "vivo.control.session.uiSwitch_state", 16);
        setInt(b, "vivo.control.seamlesscontrol", 2);
        setInt(b, "vivo.control.session.seamlesscontrol", 2);
        setInt(b, "vivo.control.EnableDCGHDR", 1);
        setInt(b, "vivo.control.echo.mode", 2);
        setInt(b, "vivo.control.videoCaptureEnable", 1);
        setInt(b, "vivo.control.lotAlgo.enable", 1);
        setInt(b, "vivo.control.twoPassNRSupport", 1);
        setInt(b, "vivo.control.distortion_correction", 1);
        setInt(b, "org.codeaurora.qcamera3.sessionParameters.enableMFNR", 1);
        setInt(b, "org.codeaurora.qcamera3.sessionParameters.EnableVSR", 1);
        setInt(b, "org.codeaurora.qcamera3.sessionParameters.numHDRexposure", 2);
        setInt(b, "org.codeaurora.qcamera3.sessionParameters.HDRModePreference", 4);
        try { b.set(new CaptureRequest.Key<>("vivo.control.currentModeEx", long[].class), new long[]{16384}); } catch (Exception ignored) { }
        setFloat(b, "vivo.control.zoom_ratio", 1.5f);
        setFloat(b, "vivo.control.initSATRatio", 0.6f);
        setFloat(b, "vivo.control.zoom.focal.length", 35f);
        b.set(CaptureRequest.CONTROL_ZOOM_RATIO, 1.5f);
        b.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, new android.util.Range<>(30, 30));
    }

    private static void photoDefaults(CaptureRequest.Builder b) {
        setInt(b, "com.vivo.current_ui_module", 1);
        setInt(b, "vivo.control.streamsUsage", usage);
        setInt(b, "vivo.control.echo.mode", 2);
        setInt(b, "vivo.control.session.uiSwitch_state", 2);
        setInt(b, "vivo.control.eis.config.enable", 5);
        setInt(b, "vivo.control.enableAutoFallback", 1);
        setInt(b, "vivo.control.MultiSportStagger", 1, 3);
        setInt(b, "vivo.control.preview.hdr.state", 2);
        setInt(b, "vivo.control.distortion_correction", 1);
        setInt(b, "vivo.control.aiae.enable", 1);
        setInt(b, "vivo.control.lotAlgo.enable", 1);
        setInt(b, "vivo.control.twoPassNRSupport", 1);
        setInt(b, "vivo.control.sceneMode", 13107200);
        setInt(b, "vivo.control.zeiss_ps", 3);
        setInt(b, "vivo.control.quickNightCapture", 1);
        setInt(b, "vivo.control.supermoon_preview_mode_for_base", 3);
        setInt(b, "org.codeaurora.qcamera3.sessionParameters.enableMFNR", 1);
        setInt(b, "org.codeaurora.qcamera3.sessionParameters.EnableVSR", 1);
        setInt(b, "org.codeaurora.qcamera3.sessionParameters.EnableAutoHDR", 1);
        setFloat(b, "vivo.control.zoom_ratio", 1.5f);
        setFloat(b, "vivo.control.initSATRatio", 1.5f);
        setFloat(b, "vivo.control.zoom.focal.length", 35f);
        b.set(CaptureRequest.CONTROL_ZOOM_RATIO, 1.5f);
    }

    private void run() throws Exception {
        final String cam = str("cam", "2");
        final int raws = getIntent().getIntExtra("raws", 1);
        final int rw = getIntent().getIntExtra("rw", 4096), rh = getIntent().getIntExtra("rh", 3072);
        final boolean rawVf = getIntent().getBooleanExtra("rawvf", false);
        final boolean still = getIntent().getBooleanExtra("still", true);
        final boolean keys = getIntent().getBooleanExtra("photokeys", true);
        final int hold = getIntent().getIntExtra("hold", 6000);
        video = getIntent().getBooleanExtra("video", false);
        if (video) usage = new int[]{3, 1, 0, 5};
        parseKeys(getIntent().getStringExtra("ikeys"), intKeys);
        parseKeys(getIntent().getStringExtra("bkeys"), byteKeys);
        String us = str("usage", "");
        if (!us.isEmpty()) usage = java.util.Arrays.stream(us.split(",")).mapToInt(Integer::parseInt).toArray();
        File dir = new File(getExternalFilesDir(null), "clone");
        dir.mkdirs();
        for (File f : dir.listFiles()) f.delete();
        final PrintWriter meta = new PrintWriter(new File(dir, "meta.txt"));
        meta.println("cam=" + cam + " raws=" + raws + " raw=" + rw + "x" + rh + " rawvf=" + rawVf + " still=" + still + " photokeys=" + keys + " usage=" + java.util.Arrays.toString(usage) + " ikeys=" + intKeys + " bkeys=" + byteKeys);
        meta.flush();

        HandlerThread cbt = new HandlerThread("clone-cb");
        cbt.start();
        final Handler cb = new Handler(cbt.getLooper());
        final Object lock = new Object();
        final CameraDevice[] device = {null};
        final CameraCaptureSession[] session = {null};
        final String[] error = {null};
        List<ImageReader> readers = new ArrayList<>();
        // Photo: yuv1440, priv1440, yuv480, yuv1920x1440, yuv4096x3072. Video: yuv1920x1080, encoder 1920x1080, yuv4096x2304
        ImageReader pvYuv = video ? ImageReader.newInstance(1920, 1080, ImageFormat.YUV_420_888, 4)
                : ImageReader.newInstance(1440, 1080, ImageFormat.YUV_420_888, 4);
        ImageReader pvPriv = video ? ImageReader.newInstance(1920, 1080, ImageFormat.PRIVATE, 6, HardwareBuffer.USAGE_VIDEO_ENCODE)
                : ImageReader.newInstance(1440, 1080, ImageFormat.PRIVATE, 4, HardwareBuffer.USAGE_GPU_SAMPLED_IMAGE);
        ImageReader small = ImageReader.newInstance(480, 360, ImageFormat.YUV_420_888, 2);
        ImageReader mid = ImageReader.newInstance(1920, 1440, ImageFormat.YUV_420_888, 2);
        ImageReader snap = video ? ImageReader.newInstance(4096, 2304, ImageFormat.YUV_420_888, 2)
                : ImageReader.newInstance(4096, 3072, ImageFormat.YUV_420_888, 2);
        readers.add(pvYuv); readers.add(pvPriv); readers.add(small); readers.add(mid); readers.add(snap);
        final int[] counts = new int[5 + raws];
        for (int i = 0; i < 5; i++) {
            final int k = i;
            readers.get(i).setOnImageAvailableListener(r -> { Image img = r.acquireNextImage(); if (img != null) { counts[k]++; img.close(); } }, cb);
        }
        final List<ImageReader> rawReaders = new ArrayList<>();
        HandlerThread wt = new HandlerThread("clone-writer");
        wt.start();
        for (int i = 0; i < raws; i++) {
            final int k = i;
            // --el rusage N: RAW reader usage; -1 (default) = ImageReader's CPU_READ_OFTEN, as vivo's own RAW reader
            long rusage = getIntent().getLongExtra("rusage", -1L);
            // --ei rfmt N: 32 RAW_SENSOR (default), 36 RAW_PRIVATE, 37 RAW10, 38 RAW12
            final int rfmt = getIntent().getIntExtra("rfmt", ImageFormat.RAW_SENSOR);
            ImageReader rr = rusage < 0 ? ImageReader.newInstance(rw, rh, rfmt, 3)
                    : ImageReader.newInstance(rw, rh, rfmt, 3, rusage);
            rr.setOnImageAvailableListener(r -> {
                Image img = r.acquireNextImage();
                if (img == null) return;
                int n = counts[5 + k]++;
                try {
                    if (n < 3 && img.getFormat() != ImageFormat.RAW_SENSOR) {
                        // packed or private layout: the whole plane buffer as is
                        Image.Plane p = img.getPlanes()[0];
                        ByteBuffer buf = p.getBuffer();
                        File out = new File(dir, "raw" + k + "_" + n + "_f" + img.getFormat() + "_" + img.getWidth() + "x" + img.getHeight()
                                + "_s" + p.getRowStride() + ".bin");
                        byte[] all = new byte[buf.remaining()];
                        buf.get(all);
                        try (FileOutputStream o = new FileOutputStream(out)) { o.write(all); }
                        synchronized (meta) { meta.println("raw" + k + " frame " + n + " fmt=" + img.getFormat() + " bytes=" + all.length + " stride=" + p.getRowStride() + " -> " + out.getName()); meta.flush(); }
                    } else if (n < 3) {
                        Image.Plane p = img.getPlanes()[0];
                        ByteBuffer buf = p.getBuffer();
                        int stride = p.getRowStride();
                        File out = new File(dir, "raw" + k + "_" + n + "_" + img.getWidth() + "x" + img.getHeight() + ".raw");
                        try (FileOutputStream o = new FileOutputStream(out)) {
                            byte[] line = new byte[img.getWidth() * 2];
                            for (int y = 0; y < img.getHeight(); y++) { buf.position(y * stride); buf.get(line); o.write(line); }
                        }
                        synchronized (meta) { meta.println("raw" + k + " frame " + n + " ts=" + img.getTimestamp() + " stride=" + stride + " -> " + out.getName()); meta.flush(); }
                    }
                } catch (Exception e) {
                    synchronized (meta) { meta.println("raw write failed: " + e); }
                } finally {
                    img.close();
                }
            }, new Handler(wt.getLooper()));
            rawReaders.add(rr);
        }

        ProbeActivity.Provider p = ProbeActivity.provider();
        final int oldSdk = p.get(SDK_KEY), oldVcf = p.get(VCF_KEY);
        meta.println("lease before " + oldSdk + "/" + oldVcf);
        try {
            CameraManager manager = getSystemService(CameraManager.class);
            manager.openCamera(cam, new CameraDevice.StateCallback() {
                @Override public void onOpened(CameraDevice d) { synchronized (lock) { device[0] = d; lock.notifyAll(); } }
                @Override public void onDisconnected(CameraDevice d) { d.close(); synchronized (lock) { error[0] = "disconnected"; lock.notifyAll(); } }
                @Override public void onError(CameraDevice d, int e) { d.close(); synchronized (lock) { error[0] = "device error " + e; lock.notifyAll(); } }
            }, cb);
            synchronized (lock) { if (device[0] == null && error[0] == null) lock.wait(5000); }
            if (device[0] == null) throw new IllegalStateException("open failed: " + error[0]);

            List<OutputConfiguration> outs = new ArrayList<>();
            // --es skip 1,2: vivo streams left out (0 yuv1440, 1 priv1440, 2 yuv480, 3 yuv1920, 4 yuv4096)
            java.util.Set<String> skip = new java.util.HashSet<>(java.util.Arrays.asList(str("skip", "").split(",")));
            for (int i = 0; i < readers.size(); i++) if (!skip.contains(String.valueOf(i))) outs.add(new OutputConfiguration(readers.get(i).getSurface()));
            meta.println("skipped streams " + skip);
            for (ImageReader r : rawReaders) outs.add(new OutputConfiguration(r.getSurface()));
            CaptureRequest.Builder sb = device[0].createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW);
            if (keys) photoKeys(sb); else extraKeys(sb);
            SessionConfiguration sc = new SessionConfiguration(SessionConfiguration.SESSION_REGULAR, outs, cb::post,
                    new CameraCaptureSession.StateCallback() {
                        @Override public void onConfigured(CameraCaptureSession s) { synchronized (lock) { session[0] = s; lock.notifyAll(); } }
                        @Override public void onConfigureFailed(CameraCaptureSession s) { synchronized (lock) { error[0] = "configure failed"; lock.notifyAll(); } }
                    });
            sc.setSessionParameters(sb.build());
            final boolean lease = getIntent().getBooleanExtra("lease", true);  // --ez lease false: configure without key 40
            if (lease) {
                p.set(VCF_KEY, 2);
                p.set(SDK_KEY, 1);
                SystemClock.sleep(100);
            }
            meta.println("key40 during configure = " + p.get(SDK_KEY));
            try {
                device[0].createCaptureSession(sc);
                synchronized (lock) { if (session[0] == null && error[0] == null) lock.wait(8000); }
            } finally {
                p.set(VCF_KEY, oldVcf);
                p.set(SDK_KEY, oldSdk);
            }
            if (session[0] == null) throw new IllegalStateException("session failed: " + error[0]);
            meta.println("session configured");
            meta.flush();

            CaptureRequest.Builder pb = device[0].createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW);
            if (!skip.contains("0")) pb.addTarget(pvYuv.getSurface());
            if (!skip.contains("1")) pb.addTarget(pvPriv.getSurface());
            if (!skip.contains("2")) pb.addTarget(small.getSurface());
            if (rawVf) for (ImageReader r : rawReaders) pb.addTarget(r.getSurface());
            // --ei iso N --el exp NS: manual exposure (AE off) on the repeating request
            final int miso = getIntent().getIntExtra("iso", -1);
            final long mexp = getIntent().getLongExtra("exp", -1L);
            // --ei evc N: AE exposure compensation steps (AE on)
            if (getIntent().hasExtra("evc")) pb.set(CaptureRequest.CONTROL_AE_EXPOSURE_COMPENSATION, getIntent().getIntExtra("evc", 0));
            if (miso > 0 && mexp > 0) {
                pb.set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_OFF);
                pb.set(CaptureRequest.SENSOR_SENSITIVITY, miso);
                pb.set(CaptureRequest.SENSOR_EXPOSURE_TIME, mexp);
                pb.set(CaptureRequest.SENSOR_FRAME_DURATION, Math.max(mexp, 33_333_333L));
                meta.println("manual iso=" + miso + " exp=" + mexp);
            }
            if (keys) photoKeys(pb); else extraKeys(pb);
            final int[] fails = {0};
            final int[] logged = {0};
            session[0].setRepeatingRequest(pb.build(), new CameraCaptureSession.CaptureCallback() {
                @Override public void onCaptureCompleted(CameraCaptureSession s, CaptureRequest r, TotalCaptureResult res) {
                    if (logged[0]++ % 30 == 10) synchronized (meta) { meta.println("preview " + sensorState(res)); meta.flush(); }
                }
                @Override public void onCaptureFailed(CameraCaptureSession s, CaptureRequest r, CaptureFailure f) {
                    if (fails[0]++ < 3) synchronized (meta) { meta.println("preview failed reason=" + f.getReason()); }
                }
            }, cb);
            meta.println("preview running; dumpsys window " + hold + " ms");
            meta.flush();
            SystemClock.sleep(hold / 2);
            if (still) {
                CaptureRequest.Builder st = device[0].createCaptureRequest(CameraDevice.TEMPLATE_STILL_CAPTURE);
                if (!skip.contains("4")) st.addTarget(snap.getSurface());
                for (ImageReader r : rawReaders) st.addTarget(r.getSurface());
                if (keys) photoKeys(st); else extraKeys(st);
                st.set(CaptureRequest.CONTROL_CAPTURE_INTENT, CaptureRequest.CONTROL_CAPTURE_INTENT_STILL_CAPTURE);
                if (getIntent().getBooleanExtra("vivostill", true)) {
                    // vivo's Photo-mode still (cmd media.camera watch, 2026-10-19): ids, sensorMode 0, isUpscale 0, isPAPNeeded 1
                    long id = System.currentTimeMillis();
                    st.set(new CaptureRequest.Key<>("vivo.control.requestId", long[].class), new long[]{id});
                    st.set(new CaptureRequest.Key<>("vivo.control.globalCaptureId", long[].class), new long[]{id + 8});
                    st.set(new CaptureRequest.Key<>("vivo.control.frameFlag", long[].class), new long[]{id});
                    setInt(st, "vivo.control.sensorMode", 0);
                    setInt(st, "vivo.control.isUpscale", 0);
                    try { st.set(new CaptureRequest.Key<>("vivo.control.isPAPNeeded", byte[].class), new byte[]{1}); } catch (Exception ignored) { }
                    meta.println("vivo still keys, id " + id);
                }
                session[0].capture(st.build(), new CameraCaptureSession.CaptureCallback() {
                    @Override public void onCaptureCompleted(CameraCaptureSession s, CaptureRequest r, TotalCaptureResult res) {
                        synchronized (meta) {
                            meta.println("still result exp=" + res.get(CaptureResult.SENSOR_EXPOSURE_TIME) + " iso=" + res.get(CaptureResult.SENSOR_SENSITIVITY) + " " + sensorState(res));
                            meta.flush();
                        }
                    }
                    @Override public void onCaptureFailed(CameraCaptureSession s, CaptureRequest r, CaptureFailure f) {
                        synchronized (meta) { meta.println("still failed reason=" + f.getReason()); meta.flush(); }
                    }
                }, cb);
            }
            SystemClock.sleep(hold / 2);
            StringBuilder c = new StringBuilder("frames per stream [yuv1440 priv1440 yuv480 yuv1920 yuv4096 raw..]:");
            for (int n : counts) c.append(' ').append(n);
            meta.println(c + " previewFailures=" + fails[0] + " error=" + error[0]);
            try { session[0].stopRepeating(); } catch (Exception ignored) { }
        } catch (Exception e) {
            meta.println("EXCEPTION " + e);
        } finally {
            if (device[0] != null) device[0].close();
            SystemClock.sleep(300);
            for (ImageReader r : readers) r.close();
            for (ImageReader r : rawReaders) r.close();
            cbt.quitSafely();
            wt.quitSafely();
            p.set(VCF_KEY, oldVcf);
            p.set(SDK_KEY, oldSdk);
            SystemClock.sleep(100);
            meta.println("lease restored " + p.get(SDK_KEY) + "/" + p.get(VCF_KEY));
            meta.println("END");
            meta.close();
        }
    }

    /** Vendor result tags (readable by name although CaptureResult.getKeys() leaves them out). */
    private static String sensorState(CaptureResult res) {
        return "mode=" + tag(res, "org.quic.camera2.properties_sensor.SensorCurrentMode", byte[].class)
                + " expCount=" + tag(res, "com.qti.stats_control.ExposureCount", int[].class)
                + " expFrames=" + tag(res, "org.quic.camera.hdrfeaturemode.NumberOfExpFrames", int[].class)
                + " vc=" + tag(res, "org.quic.camera2.properties_sensor.SensorVirtualChannelInfo", byte[].class)
                + " dcg=" + tag(res, "com.qti.stats_control.DCGMode", int[].class)
                + " gains=" + tag(res, "org.quic.camera2.statsconfigs.AECLinearGain", float[].class)
                + " msiTs=" + tag(res, "vivo.feedback.matchedMSIRawTimeStamp", long[].class)
                + " spectral=" + tag(res, "vivo.feedback.spectralInfo", float[].class)
                + " aiawb=" + tag(res, "vivo.feedback.AIAWBInfo", float[].class)
                + " lux=" + tag(res, "org.quic.camera2.statsconfigs.AECLuxIndex", float[].class)
                + " adrc=" + tag(res, "org.quic.camera2.statsconfigs.AECCompenADRCGain", float[].class)
                + " dark=" + tag(res, "org.quic.camera2.statsconfigs.AECCompenDarkBoostGain", float[].class)
                + " hdrScene=" + tag(res, "org.codeaurora.qcamera3.stats.is_hdr_scene", byte[].class)
                + " ccGains=" + res.get(CaptureResult.COLOR_CORRECTION_GAINS)
                + " neutral=" + ProbeActivity.format(res.get(CaptureResult.SENSOR_NEUTRAL_COLOR_POINT))
                + " hdrFeature=" + tag(res, "org.quic.camera.hdrfeaturemode.HDRFeatureMode", int[].class)
                + " idealRaw=" + tag(res, "com.qti.chi.rawcbinfo.IdealRaw", byte[].class)
                + " exp=" + res.get(CaptureResult.SENSOR_EXPOSURE_TIME) + " iso=" + res.get(CaptureResult.SENSOR_SENSITIVITY);
    }

    @SuppressWarnings({"unchecked", "rawtypes"})
    private static String tag(CaptureResult res, String name, Class<?> type) {
        try { return ProbeActivity.format(res.get(new CaptureResult.Key(name, type))); } catch (Exception e) { return "n/a"; }
    }

    private String str(String k, String d) {
        String v = getIntent().getStringExtra(k);
        return v == null ? d : v;
    }
}
