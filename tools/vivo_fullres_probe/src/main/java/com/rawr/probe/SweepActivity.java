package com.rawr.probe;

import android.app.Activity;
import android.content.SharedPreferences;
import android.graphics.ImageFormat;
import android.graphics.SurfaceTexture;
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
import android.text.InputType;
import android.util.Log;
import android.util.Range;
import android.view.Surface;
import android.view.TextureView;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.OutputStream;
import java.io.PrintWriter;
import java.nio.ByteBuffer;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.TimeUnit;

/**
 * DCG / sensor-mode sweep with a viewfinder: frame a high-contrast scene (tripod), press Capture. For every selected
 * SensorModeTable row the camera is reconfigured with vivo.control.forceSensorMode (key 40 held only around the
 * session configure) and streams a fixed-ISO exposure bracket t/8, t/2, t, t, 2t, 8t, with t from the preview AE.
 * Per row: the first t frame in full (Bayer), every frame's green plane (half size, unbinned), the full result
 * metadata of the t frame. tools/vivo_fullres_probe/pull_dcg.sh fetches the newest run, analyze_dcg.py reads it.
 */
public final class SweepActivity extends Activity implements TextureView.SurfaceTextureListener {
    private static final String TAG = "RAWRSWEEP";
    private static final int SDK_KEY = 40, VCF_KEY = 33;
    private static final String MODE_TABLE = "org.quic.camera2.sensormode.info.SensorModeTable";
    private static final CaptureRequest.Key<int[]> FORCE_MODE = new CaptureRequest.Key<>("vivo.control.forceSensorMode", int[].class);
    /** Bracket as multiples of t; the doubled 1.0 is the temporal-noise pair. */
    private static final double[] BRACKET = {0.125, 0.5, 1.0, 1.0, 2.0, 8.0};
    private static final long MAX_EXPOSURE_NS = 400_000_000L;
    /** Vendor result tags read explicitly as well, in case getKeys() leaves them out. */
    private static final String[] VENDOR_RESULTS = {
            "vivo.parameter.VivoLaserData", "com.qti.chi.statsaec.AecLux", "vivo.parameter.aec_settled",
            "vivo.parameter.aeLuxIndexValue", "vivo.feedback.aeluxindexvalue", "vivo.parameter.captureOisData",
            "vivo.feedback.NoiseReductionApplied", "vivo.feedback.mergeNum", "vivo.feedback.ISOGain",
            "vivo.feedback.sensorMode", "vivo.control.forceSensorMode", "vivo.feedback.open_laser",
            "vivo.parameter.rawHDRParams", "vivo.feedback.rawhdrISPAEParams", "vivo.parameter.hdrProcessType",
            "org.quic.camera2.statsconfigs.AECLuxIndex", "vcf.parameter.seamlessMode", "vivo.control.originalSensorModeIndex",
            "org.quic.camera.SensorModeFS.isFastShutterModeSupported", "com.vivo.current_ui_module"};

    private TextureView viewfinder;
    private TextView status;
    private EditText camField, rowsField, evField;
    private Button go;

    private HandlerThread camThread, cbThread;
    /** cam runs the blocking open/configure helpers; camera callbacks arrive on cb so they never wait on them. */
    private Handler cam, cb;
    private CameraManager manager;
    private CameraDevice device;
    private CameraCaptureSession session;
    private String openId;
    private volatile TotalCaptureResult lastPreview;
    /** Set when the open device reports an error or disconnect: the sweep reopens before the next row. */
    private volatile String deviceError;
    private volatile boolean abort;
    private Thread worker;

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        manager = getSystemService(CameraManager.class);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        viewfinder = new TextureView(this);
        int width = getResources().getDisplayMetrics().widthPixels;
        root.addView(viewfinder, new LinearLayout.LayoutParams(width, width * 4 / 3));
        LinearLayout fields = new LinearLayout(this);
        camField = field("cam", "3", InputType.TYPE_CLASS_NUMBER);
        rowsField = field("rows (auto or 4,17,26)", "auto", InputType.TYPE_CLASS_TEXT);
        evField = field("EV", "0", InputType.TYPE_CLASS_NUMBER | InputType.TYPE_NUMBER_FLAG_SIGNED | InputType.TYPE_NUMBER_FLAG_DECIMAL);
        fields.addView(camField, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        fields.addView(rowsField, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 3));
        fields.addView(evField, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        root.addView(fields);
        go = new Button(this);
        go.setText("Capture sweep");
        go.setOnClickListener(v -> startSweep());
        root.addView(go);
        status = new TextView(this);
        status.setText("Tripod, high-contrast scene (bright sky / lamp + deep shadow). Lens stays still during the sweep.");
        root.addView(status);
        setContentView(root);
        getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);  // a screen timeout would abort the sweep
        viewfinder.setSurfaceTextureListener(this);
        camThread = new HandlerThread("sweep-cam");
        camThread.start();
        cam = new Handler(camThread.getLooper());
        cbThread = new HandlerThread("sweep-cb");
        cbThread.start();
        cb = new Handler(cbThread.getLooper());
        recoverLease();
    }

    private EditText field(String hint, String value, int type) {
        EditText e = new EditText(this);
        e.setHint(hint);
        e.setText(value);
        e.setInputType(type);
        e.setSingleLine(true);
        return e;
    }

    private void say(String s) {
        Log.i(TAG, s);
        runOnUiThread(() -> status.setText(s));
    }

    // ---- lifecycle: the camera only lives while the screen is in front ---------------------------------------------

    @Override protected void onResume() {
        super.onResume();
        if (viewfinder.isAvailable()) cam.post(this::openPreview);
    }

    @Override protected void onPause() {
        abort = true;
        if (worker != null) {
            try { worker.join(5000); } catch (InterruptedException ignored) { }
            worker = null;
        }
        cam.post(this::closeCamera);
        super.onPause();
    }

    @Override protected void onDestroy() {
        camThread.quitSafely();
        cbThread.quitSafely();
        super.onDestroy();
    }

    @Override public void onSurfaceTextureAvailable(SurfaceTexture s, int w, int h) { cam.post(this::openPreview); }
    @Override public void onSurfaceTextureSizeChanged(SurfaceTexture s, int w, int h) { }
    @Override public boolean onSurfaceTextureDestroyed(SurfaceTexture s) { return true; }
    @Override public void onSurfaceTextureUpdated(SurfaceTexture s) { }

    // ---- key 40 lease, recovered at the next start if the process died while holding it ------------------------------

    private SharedPreferences leasePrefs() { return getSharedPreferences("lease", MODE_PRIVATE); }

    private void leaseOn() throws Exception {
        ProbeActivity.Provider p = ProbeActivity.provider();
        SharedPreferences sp = leasePrefs();
        if (!sp.contains("sdk")) sp.edit().putInt("sdk", p.get(SDK_KEY)).putInt("vcf", p.get(VCF_KEY)).commit();
        p.set(VCF_KEY, 2);
        p.set(SDK_KEY, 1);
        SystemClock.sleep(100);
    }

    private void leaseOff() {
        SharedPreferences sp = leasePrefs();
        if (!sp.contains("sdk")) return;
        try {
            ProbeActivity.Provider p = ProbeActivity.provider();
            p.set(VCF_KEY, sp.getInt("vcf", 2));
            p.set(SDK_KEY, sp.getInt("sdk", 0));
            sp.edit().clear().commit();
        } catch (Throwable t) {
            Log.e(TAG, "lease restore failed", t);
        }
    }

    private void recoverLease() {
        if (leasePrefs().contains("sdk")) {
            leaseOff();
            Log.w(TAG, "restored key 40 left over from a killed run");
        }
    }

    // ---- camera plumbing (blocking helpers run off the camera thread) -------------------------------------------------

    /** Errors with stack traces in files/sweep_errors.txt: vivo mutes app logcat after heavy logging. */
    private void report(String what, Throwable t) {
        Log.e(TAG, what, t);
        try (PrintWriter out = new PrintWriter(new java.io.FileWriter(new File(getExternalFilesDir(null), "sweep_errors.txt"), true))) {
            out.println(new Date() + " " + what);
            t.printStackTrace(out);
        } catch (Exception ignored) { }
    }

    /** Viewfinder on the sweep camera, else on public camera 0 (the same main sensor at 1x). */
    private void openPreview() {
        if (worker != null) return;
        String want = camField.getText().toString().trim();
        StringBuilder errors = new StringBuilder();
        for (String id : want.equals("0") ? new String[]{"0"} : new String[]{want, "0"}) {
            try {
                startPreview(id);
                if (!id.equals(want)) say("viewfinder on cam 0 (cam " + want + ": " + errors + "); the sweep still uses cam " + want);
                return;
            } catch (Throwable t) {
                report("preview on cam " + id, t);
                errors.append(t.getClass().getSimpleName()).append(": ").append(t.getMessage()).append(' ');
                closeCamera();
            }
        }
        say("preview failed: " + errors + "\n(details in files/sweep_errors.txt)");
    }

    private void startPreview(String id) throws Exception {
        if (device == null || !id.equals(openId)) {
            closeCamera();
            device = openBlocking(id);
            openId = id;
        }
        SurfaceTexture st = viewfinder.getSurfaceTexture();
        if (st == null) return;
        st.setDefaultBufferSize(1440, 1080);
        Surface surface = new Surface(st);
        session = configureBlocking(Collections.singletonList(surface), null);
        CaptureRequest.Builder b = device.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW);
        b.addTarget(surface);
        b.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_PICTURE);
        final int[] n = {0};
        session.setRepeatingRequest(b.build(), new CameraCaptureSession.CaptureCallback() {
            @Override public void onCaptureCompleted(CameraCaptureSession s, CaptureRequest r, TotalCaptureResult res) {
                lastPreview = res;
                if (n[0]++ % 15 == 0 && worker == null)
                    say(String.format(Locale.US, "cam %s preview  AE %.2f ms  ISO %s  focus %s dpt", openId,
                            nz(res.get(CaptureResult.SENSOR_EXPOSURE_TIME)) / 1e6, res.get(CaptureResult.SENSOR_SENSITIVITY),
                            res.get(CaptureResult.LENS_FOCUS_DISTANCE)));
            }
        }, cb);
    }

    private void closeCamera() {
        if (session != null) { try { session.close(); } catch (Exception ignored) { } session = null; }
        if (device != null) { device.close(); device = null; }
        openId = null;
    }

    private CameraDevice openBlocking(String id) throws Exception {
        final Object lock = new Object();
        final CameraDevice[] d = {null};
        final String[] err = {null};
        manager.openCamera(id, new CameraDevice.StateCallback() {
            @Override public void onOpened(CameraDevice c) { synchronized (lock) { d[0] = c; lock.notifyAll(); } }
            @Override public void onDisconnected(CameraDevice c) { c.close(); deviceError = "disconnected"; synchronized (lock) { err[0] = "disconnected"; lock.notifyAll(); } }
            @Override public void onError(CameraDevice c, int e) { c.close(); deviceError = "device error " + e; synchronized (lock) { err[0] = "error " + e; lock.notifyAll(); } }
        }, cb);
        synchronized (lock) { if (d[0] == null && err[0] == null) lock.wait(5000); }
        if (d[0] == null) throw new IllegalStateException("open " + id + " failed: " + err[0]);
        deviceError = null;
        return d[0];
    }

    private CameraCaptureSession configureBlocking(List<Surface> outputs, CaptureRequest params) throws Exception {
        final Object lock = new Object();
        final CameraCaptureSession[] s = {null};
        final String[] err = {null};
        List<OutputConfiguration> configs = new ArrayList<>();
        for (Surface o : outputs) configs.add(new OutputConfiguration(o));
        SessionConfiguration sc = new SessionConfiguration(SessionConfiguration.SESSION_REGULAR, configs, cb::post,
                new CameraCaptureSession.StateCallback() {
                    @Override public void onConfigured(CameraCaptureSession c) { synchronized (lock) { s[0] = c; lock.notifyAll(); } }
                    @Override public void onConfigureFailed(CameraCaptureSession c) { synchronized (lock) { err[0] = "configure failed"; lock.notifyAll(); } }
                });
        if (params != null) sc.setSessionParameters(params);
        device.createCaptureSession(sc);
        synchronized (lock) { if (s[0] == null && err[0] == null) lock.wait(8000); }
        if (s[0] == null) throw new IllegalStateException(err[0] == null ? "configure timeout" : err[0]);
        return s[0];
    }

    // ---- the sweep ------------------------------------------------------------------------------------------------------

    private void startSweep() {
        if (worker != null || device == null || lastPreview == null) return;
        abort = false;
        go.setEnabled(false);
        final String rows = rowsField.getText().toString().trim();
        final String sweepId = camField.getText().toString().trim();
        final double ev = parseDouble(evField.getText().toString(), 0);
        final TotalCaptureResult ae = lastPreview;
        worker = new Thread(() -> {
            try {
                sweep(sweepId, rows, ev, ae);
            } catch (Throwable t) {
                report("sweep", t);
                say("sweep failed: " + t);
            } finally {
                leaseOff();
                worker = null;
                runOnUiThread(() -> go.setEnabled(true));
                if (!abort) cam.post(this::openPreview);
            }
        }, "sweep");
        worker.start();
    }

    private static final class Row {
        final int index, w, h, fps;
        /** Selector mode (no forceSensorMode): the vivo UI module to claim, a YUV preview stream, EnableDCGHDR. */
        final int ui;
        final boolean preview, dcg;
        final String label;
        Row(int index, int w, int h, int fps) { this(index, w, h, fps, -1, false, false, String.valueOf(index)); }
        Row(int index, int w, int h, int fps, int ui, boolean preview, boolean dcg, String label) {
            this.index = index; this.w = w; this.h = h; this.fps = fps; this.ui = ui; this.preview = preview; this.dcg = dcg; this.label = label;
        }
        boolean forced() { return ui < 0; }
    }

    /**
     * "sel" tokens leave the mode to the HAL's selector under key 40 (AppType vivo), as vivo's camera gets DHDR2 / DCG1_16:
     * sel&lt;ui&gt;[p][d] = com.vivo.current_ui_module &lt;ui&gt;, p a 1440x1080 YUV preview stream beside the RAW, d
     * vivo.control.EnableDCGHDR 1. Written as rows 100+ in the files; rows.txt keeps the token.
     */
    private static Row selectorRow(String token, int k) {
        String t = token.substring(3);
        int n = 0;
        while (n < t.length() && Character.isDigit(t.charAt(n))) n++;
        int ui = n > 0 ? Integer.parseInt(t.substring(0, n)) : 1;
        String flags = t.substring(n);
        return new Row(100 + k, 4080, 3064, 30, ui, flags.contains("p"), flags.contains("d"), token);
    }

    private void sweep(String id, String rowSpec, double ev, TotalCaptureResult ae) throws Exception {
        CameraCharacteristics ch = manager.getCameraCharacteristics(id);
        int[] table = ch.get(new CameraCharacteristics.Key<>(MODE_TABLE, int[].class));
        List<Row> all = new ArrayList<>();
        if (table != null && table.length > 2) {
            int count = table[0], stride = table[1];
            for (int k = 0; k < count && 2 + k * stride + 2 < table.length; k++)
                all.add(new Row(k, table[2 + k * stride], table[3 + k * stride], table[4 + k * stride]));
        }
        List<Row> rows = new ArrayList<>();
        if (rowSpec.equals("sel")) {
            // vivo photo-mode UI modules from the selector's DHDR2 branch, RAW alone and beside a preview
            String[] preset = {"sel1", "sel1p", "sel1pd", "sel7p", "sel13p", "sel74p"};
            for (int k = 0; k < preset.length; k++) rows.add(selectorRow(preset[k], k));
        } else if (rowSpec.isEmpty() || rowSpec.equals("auto")) {
            // the binned-size rows (8-14 MP) up to 60 fps: every DCG / stagger / ISZ candidate of a 50 MP sensor
            for (Row r : all) if ((long) r.w * r.h >= 7_000_000L && (long) r.w * r.h <= 14_000_000L && r.fps <= 60) rows.add(r);
        } else {
            int selectors = 0;
            for (String s : rowSpec.split(",")) {
                if (s.trim().startsWith("sel")) { rows.add(selectorRow(s.trim(), selectors++)); continue; }
                int k = Integer.parseInt(s.trim());
                if (k >= 0 && k < all.size()) rows.add(all.get(k));
            }
        }
        if (rows.isEmpty()) { say("no rows to sweep (table " + all.size() + " rows)"); return; }

        Range<Integer> isoRange = ch.get(CameraCharacteristics.SENSOR_INFO_SENSITIVITY_RANGE);
        Range<Long> expRange = ch.get(CameraCharacteristics.SENSOR_INFO_EXPOSURE_TIME_RANGE);
        int iso = isoRange.getLower();
        double aeExp = nz(ae.get(CaptureResult.SENSOR_EXPOSURE_TIME));
        int aeIso = ae.get(CaptureResult.SENSOR_SENSITIVITY) == null ? iso : ae.get(CaptureResult.SENSOR_SENSITIVITY);
        Float focus = ae.get(CaptureResult.LENS_FOCUS_DISTANCE);
        long t = (long) (aeExp * aeIso / iso * Math.pow(2, ev));
        t = Math.max(expRange.getLower() * 8, Math.min(t, MAX_EXPOSURE_NS / 8));
        Integer cfa = ch.get(CameraCharacteristics.SENSOR_INFO_COLOR_FILTER_ARRANGEMENT);
        // a green site of the 2x2 cell: (0,1) for RGGB/BGGR, (0,0) for GRBG/GBRG
        int gx = (cfa != null && (cfa == 1 || cfa == 2)) ? 0 : 1, gy = 0;

        File dir = new File(getExternalFilesDir(null), "dcg_" + new SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(new Date()));
        dir.mkdirs();
        try (PrintWriter out = new PrintWriter(new File(dir, "chars.txt"))) {
            for (CameraCharacteristics.Key<?> key : ch.getKeys()) {
                Object v;
                try { v = ch.get(key); } catch (Exception e) { v = "<" + e + ">"; }
                out.println(key.getName() + " = " + ProbeActivity.format(v));
            }
            out.println(MODE_TABLE + " = " + ProbeActivity.format(table));
        }
        try (PrintWriter out = new PrintWriter(new File(dir, "sweep.txt"))) {
            out.println("cam=" + id + " cfa=" + cfa + " green=(" + gx + "," + gy + ") iso=" + iso + " t=" + t + " ev=" + ev
                    + " focus=" + focus + " previewAE exp=" + (long) aeExp + " iso=" + aeIso + " pkg=" + getPackageName());
            out.println("bracket x t: " + java.util.Arrays.toString(BRACKET));
            StringBuilder b = new StringBuilder("rows:");
            for (Row r : rows) b.append(' ').append(r.label).append('=').append(r.w).append('x').append(r.h).append('@').append(r.fps);
            out.println(b);
            dumpResult(out, "preview result", ae);
        }
        PrintWriter csv = new PrintWriter(new File(dir, "frames.csv"));
        csv.println("row,format,w,h,fps,kind,file,mult,exp_req,exp,iso,black,white,ts,frame,sensor_mode");
        PrintWriter log = new PrintWriter(new File(dir, "rows.txt"));

        try {
            if (session != null) { try { session.stopRepeating(); } catch (Exception ignored) { } }
            int done = 0;
            for (int i = 0; i < rows.size() && !abort; i++) {
                Row r = rows.get(i);
                if (!reopenIfNeeded(id, log)) { log.println("camera " + id + " would not reopen; stopping"); break; }
                say(String.format(Locale.US, "row %s (%dx%d@%d)  %d/%d", r.label, r.w, r.h, r.fps, i + 1, rows.size()));
                long t0 = SystemClock.elapsedRealtime();
                String result;
                try {
                    result = sweepRow(dir, csv, r, ImageFormat.RAW_SENSOR, iso, t, focus, gx, gy);
                    done++;
                } catch (Exception e) {
                    result = "FAILED " + e + (deviceError != null ? " [" + deviceError + "]" : "");
                    report("row " + r.index, e);
                }
                log.println("row " + r.label + (r.forced() ? "" : " (file row " + r.index + ")") + " " + r.w + "x" + r.h + "@" + r.fps + ": " + result + " (" + (SystemClock.elapsedRealtime() - t0) + " ms)");
                log.flush();
                csv.flush();
            }
            say((abort ? "ABORTED " : "Done ") + done + "/" + rows.size() + " rows -> " + dir.getName() + "\nrun tools/vivo_fullres_probe/pull_dcg.sh");
        } finally {
            csv.close();
            log.println(abort ? "ABORTED" : "END");
            log.close();
        }
    }

    /** A row that stalls can make the HAL drop the device: reopen so the remaining rows still run. */
    private boolean reopenIfNeeded(String id, PrintWriter log) {
        if (device != null && deviceError == null && id.equals(openId)) return true;
        String why = deviceError;
        closeCamera();
        for (int attempt = 0; attempt < 3 && device == null && !abort; attempt++) {
            SystemClock.sleep(attempt == 0 && why == null ? 0 : 1500);
            try {
                device = openBlocking(id);
                openId = id;
            } catch (Exception e) {
                report("reopen cam " + id, e);
            }
        }
        if (why != null) log.println("  (camera reopened after: " + why + ")");
        return device != null;
    }

    /** Forced rows: forceSensorMode. Selector rows: the claimed UI module (and EnableDCGHDR), no forced mode. */
    private static void modeKeys(CaptureRequest.Builder b, Row r) {
        if (r.forced()) { b.set(FORCE_MODE, new int[]{r.index}); return; }
        b.set(new CaptureRequest.Key<>("com.vivo.current_ui_module", int[].class), new int[]{r.ui});
        if (r.dcg) {
            try { b.set(new CaptureRequest.Key<>("vivo.control.EnableDCGHDR", int[].class), new int[]{1}); } catch (Exception ignored) { }
        }
    }

    private String sweepRow(File dir, PrintWriter csv, Row r, int format, int iso, long t, Float focus, int gx, int gy) throws Exception {
        final BlockingQueue<Image> images = new ArrayBlockingQueue<>(3);
        final Map<Long, TotalCaptureResult> results = new ConcurrentHashMap<>();
        final int[] failures = {0};
        ImageReader reader = ImageReader.newInstance(r.w, r.h, format, 5, 0L);
        final ImageReader preview = r.preview ? ImageReader.newInstance(1440, 1080, ImageFormat.YUV_420_888, 3) : null;
        if (preview != null) preview.setOnImageAvailableListener(rd -> { Image i = rd.acquireNextImage(); if (i != null) i.close(); }, cb);
        reader.setOnImageAvailableListener(rd -> {
            Image img = rd.acquireNextImage();
            if (img != null && !images.offer(img)) img.close();
        }, cb);
        try {
            CaptureRequest.Builder sb = device.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW);
            sb.set(CaptureRequest.CONTROL_MODE, CaptureRequest.CONTROL_MODE_AUTO);
            sb.set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_ON);
            modeKeys(sb, r);
            sb.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, new Range<>(Math.min(15, r.fps), r.fps));
            leaseOn();
            try {
                session = configureBlocking(preview == null ? Collections.singletonList(reader.getSurface())
                        : java.util.Arrays.asList(preview.getSurface(), reader.getSurface()), sb.build());
            } finally {
                leaseOff();
            }
            CaptureRequest.Builder b = device.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW);
            b.addTarget(reader.getSurface());
            if (preview != null) b.addTarget(preview.getSurface());
            b.set(CaptureRequest.CONTROL_MODE, CaptureRequest.CONTROL_MODE_AUTO);
            b.set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_OFF);
            b.set(CaptureRequest.SENSOR_SENSITIVITY, iso);
            b.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_OFF);
            if (focus != null) b.set(CaptureRequest.LENS_FOCUS_DISTANCE, focus);
            modeKeys(b, r);
            CameraCaptureSession.CaptureCallback callback = new CameraCaptureSession.CaptureCallback() {
                @Override public void onCaptureCompleted(CameraCaptureSession s, CaptureRequest q, TotalCaptureResult res) {
                    Long ts = res.get(CaptureResult.SENSOR_TIMESTAMP);
                    if (ts != null) results.put(ts, res);
                }
                @Override public void onCaptureFailed(CameraCaptureSession s, CaptureRequest q, CaptureFailure f) { failures[0]++; }
            };
            StringBuilder summary = new StringBuilder();
            boolean fullSaved = false;
            for (int k = 0; k < BRACKET.length && !abort; k++) {
                long exp = (long) (t * BRACKET[k]);
                boolean repeat = k > 0 && BRACKET[k] == BRACKET[k - 1];
                if (!repeat) {
                    b.set(CaptureRequest.SENSOR_EXPOSURE_TIME, exp);
                    b.set(CaptureRequest.SENSOR_FRAME_DURATION, Math.max(exp, 1_000_000_000L / r.fps));
                    session.setRepeatingRequest(b.build(), callback, cb);
                }
                // settle: two frames after the change and the result exposure within 3 % (or give up after 10 frames)
                Image img = null;
                TotalCaptureResult res = null;
                for (int n = 0; n < 12; n++) {
                    Image cand = images.poll(3, TimeUnit.SECONDS);
                    if (cand == null) throw new IllegalStateException("no frame (" + failures[0] + " capture failures)");
                    TotalCaptureResult cr = waitResult(results, cand.getTimestamp());
                    Long got = cr == null ? null : cr.get(CaptureResult.SENSOR_EXPOSURE_TIME);
                    boolean match = got != null && Math.abs(got - exp) <= exp * 0.03;
                    if ((repeat || n >= 2) && (match || n >= 10)) { img = cand; res = cr; break; }
                    cand.close();
                }
                if (img == null) throw new IllegalStateException("no settled frame at " + exp);
                try {
                    String base = String.format(Locale.US, "r%02d%s_%d", r.index, format == ImageFormat.RAW10 ? "_raw10" : "", k);
                    if (BRACKET[k] == 1.0 && !fullSaved) {
                        writeFull(img, new File(dir, base + "_full.raw"));
                        csvLine(csv, r, format, "full", base + "_full.raw", BRACKET[k], exp, res, img);
                        try (PrintWriter out = new PrintWriter(new File(dir, String.format(Locale.US, "r%02d%s_result.txt", r.index, format == ImageFormat.RAW10 ? "_raw10" : "")))) {
                            dumpResult(out, "row " + r.index + " t frame", res);
                        }
                        fullSaved = true;
                    }
                    writeGreen(img, gx, gy, new File(dir, base + "_g.raw"));
                    csvLine(csv, r, format, "green", base + "_g.raw", BRACKET[k], exp, res, img);
                    Long got = res == null ? null : res.get(CaptureResult.SENSOR_EXPOSURE_TIME);
                    summary.append(String.format(Locale.US, "%.3gt=%s ", BRACKET[k], got == null ? "?" : String.format(Locale.US, "%.2fms", got / 1e6)));
                } finally {
                    img.close();
                }
            }
            try { session.stopRepeating(); } catch (Exception ignored) { }
            return "ok " + summary + "failures=" + failures[0];
        } finally {
            Image left;
            while ((left = images.poll()) != null) left.close();
            if (session != null) { try { session.close(); } catch (Exception ignored) { } session = null; }
            SystemClock.sleep(200);
            reader.close();
            if (preview != null) preview.close();
        }
    }

    private static TotalCaptureResult waitResult(Map<Long, TotalCaptureResult> results, long ts) throws InterruptedException {
        for (int i = 0; i < 50; i++) {
            TotalCaptureResult r = results.get(ts);
            if (r != null) return r;
            Thread.sleep(20);
        }
        return null;
    }

    private static void csvLine(PrintWriter csv, Row r, int format, String kind, String file, double mult, long expReq, TotalCaptureResult res, Image img) {
        float[] black = res == null ? null : res.get(CaptureResult.SENSOR_DYNAMIC_BLACK_LEVEL);
        StringBuilder bl = new StringBuilder();
        if (black != null) for (int i = 0; i < black.length; i++) bl.append(i > 0 ? ";" : "").append(black[i]);
        csv.println(r.index + "," + (format == ImageFormat.RAW10 ? "raw10" : "raw16") + "," + img.getWidth() + "," + img.getHeight() + "," + r.fps + "," + kind + "," + file + "," + mult + "," + expReq
                + "," + (res == null ? "" : res.get(CaptureResult.SENSOR_EXPOSURE_TIME))
                + "," + (res == null ? "" : res.get(CaptureResult.SENSOR_SENSITIVITY))
                + "," + bl + "," + (res == null ? "" : res.get(CaptureResult.SENSOR_DYNAMIC_WHITE_LEVEL))
                + "," + img.getTimestamp() + "," + (res == null ? "" : res.getFrameNumber())
                + "," + (res == null ? "" : vendorString(res, "vivo.feedback.sensorMode").replace(',', ';')));
    }

    /** Row y as 16-bit samples: RAW_SENSOR copied, RAW10 (4 pixels in 5 bytes) unpacked. */
    private static void readRow(Image img, int y, byte[] packed, short[] out) {
        Image.Plane plane = img.getPlanes()[0];
        ByteBuffer buf = plane.getBuffer();
        int w = img.getWidth();
        buf.position(y * plane.getRowStride());
        if (img.getFormat() == ImageFormat.RAW10) {
            buf.get(packed, 0, w / 4 * 5);
            for (int x = 0; x < w; x++) {
                int g = (x >> 2) * 5, i = x & 3;
                out[x] = (short) (((packed[g + i] & 0xff) << 2) | (((packed[g + 4] & 0xff) >> (2 * i)) & 3));
            }
        } else {
            buf.get(packed, 0, w * 2);
            for (int x = 0; x < w; x++) out[x] = (short) ((packed[2 * x] & 0xff) | ((packed[2 * x + 1] & 0xff) << 8));
        }
    }

    private static void writeShorts(OutputStream o, short[] v, int n, byte[] scratch) throws Exception {
        for (int i = 0; i < n; i++) { scratch[2 * i] = (byte) v[i]; scratch[2 * i + 1] = (byte) (v[i] >> 8); }
        o.write(scratch, 0, 2 * n);
    }

    /** Whole frame, uint16 little-endian, rows packed (stride removed, RAW10 unpacked). */
    private static void writeFull(Image img, File file) throws Exception {
        int w = img.getWidth(), h = img.getHeight();
        byte[] packed = new byte[w * 2], scratch = new byte[w * 2];
        short[] line = new short[w];
        try (OutputStream o = new java.io.BufferedOutputStream(new FileOutputStream(file), 1 << 20)) {
            for (int y = 0; y < h; y++) { readRow(img, y, packed, line); writeShorts(o, line, w, scratch); }
        }
    }

    /** One green site of every 2x2 cell (w/2 x h/2, uint16 LE): raw values, no averaging, so noise statistics hold. */
    private static void writeGreen(Image img, int gx, int gy, File file) throws Exception {
        int w = img.getWidth(), h = img.getHeight();
        byte[] packed = new byte[w * 2], scratch = new byte[w];
        short[] line = new short[w], half = new short[w / 2];
        try (OutputStream o = new java.io.BufferedOutputStream(new FileOutputStream(file), 1 << 20)) {
            for (int y = gy; y < h; y += 2) {
                readRow(img, y, packed, line);
                for (int x = 0; x < w / 2; x++) half[x] = line[2 * x + gx];
                writeShorts(o, half, w / 2, scratch);
            }
        }
    }

    private static void dumpResult(PrintWriter out, String title, CaptureResult res) {
        out.println("## " + title);
        if (res == null) return;
        java.util.Set<String> seen = new java.util.HashSet<>();
        for (CaptureResult.Key<?> key : res.getKeys()) {
            Object v;
            try { v = res.get(key); } catch (Exception e) { v = "<" + e + ">"; }
            seen.add(key.getName());
            out.println(key.getName() + " = " + ProbeActivity.format(v));
        }
        for (String name : VENDOR_RESULTS) if (!seen.contains(name)) out.println(name + " = " + vendorString(res, name));
    }

    /** A vendor tag of unknown type: tried as int32, int64, float, double, byte. */
    private static String vendorString(CaptureResult res, String name) {
        Class<?>[] types = {int[].class, long[].class, float[].class, double[].class, byte[].class};
        for (Class<?> type : types) {
            try {
                @SuppressWarnings({"unchecked", "rawtypes"})
                Object v = res.get(new CaptureResult.Key(name, type));
                if (v != null) return ProbeActivity.format(v);
            } catch (Exception ignored) { }
        }
        return "n/a";
    }

    private static double nz(Long v) { return v == null ? 0 : v; }

    private static double parseDouble(String s, double fallback) {
        try { return Double.parseDouble(s.trim()); } catch (Exception e) { return fallback; }
    }
}
