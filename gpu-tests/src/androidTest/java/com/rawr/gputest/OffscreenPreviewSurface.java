package com.rawr.gputest;

import android.graphics.SurfaceTexture;
import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLSurface;
import android.opengl.GLES11Ext;
import android.opengl.GLES20;
import android.os.Handler;
import android.os.HandlerThread;
import android.view.Surface;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

/** Drains Vulkan presentation on an EGL thread, with no Activity or display UI. */
final class OffscreenPreviewSurface implements AutoCloseable {
    final AtomicInteger frames = new AtomicInteger();
    final AtomicReference<Throwable> failure = new AtomicReference<>();
    private final HandlerThread thread = new HandlerThread("headless-egl-consumer");
    private final Handler handler;
    private EGLDisplay display = EGL14.EGL_NO_DISPLAY;
    private EGLContext context = EGL14.EGL_NO_CONTEXT;
    private EGLSurface pbuffer = EGL14.EGL_NO_SURFACE;
    private SurfaceTexture texture;
    private Surface surface;
    OffscreenPreviewSurface() throws Exception {
        thread.start(); handler = new Handler(thread.getLooper());
        CountDownLatch ready = new CountDownLatch(1);
        handler.post(() -> {
            try {
                display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
                int[] version = new int[2];
                require(EGL14.eglInitialize(display, version, 0, version, 1), "eglInitialize");
                int[] attributes = {EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8, EGL14.EGL_BLUE_SIZE, 8,
                    EGL14.EGL_ALPHA_SIZE, 8, EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
                    EGL14.EGL_SURFACE_TYPE, EGL14.EGL_PBUFFER_BIT, EGL14.EGL_NONE};
                EGLConfig[] configs = new EGLConfig[1]; int[] count = new int[1];
                require(EGL14.eglChooseConfig(display, attributes, 0, configs, 0, 1, count, 0) && count[0] > 0, "eglChooseConfig");
                context = EGL14.eglCreateContext(display, configs[0], EGL14.EGL_NO_CONTEXT,
                    new int[]{EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE}, 0);
                require(context != EGL14.EGL_NO_CONTEXT, "eglCreateContext");
                pbuffer = EGL14.eglCreatePbufferSurface(display, configs[0],
                    new int[]{EGL14.EGL_WIDTH, 1, EGL14.EGL_HEIGHT, 1, EGL14.EGL_NONE}, 0);
                require(EGL14.eglMakeCurrent(display, pbuffer, pbuffer, context), "eglMakeCurrent");
                int[] names = new int[1]; GLES20.glGenTextures(1, names, 0);
                GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, names[0]);
                texture = new SurfaceTexture(names[0]); texture.setDefaultBufferSize(720, 960);
                texture.setOnFrameAvailableListener(t -> {
                    try { t.updateTexImage(); frames.incrementAndGet(); }
                    catch (Throwable error) { failure.compareAndSet(null, error); }
                }, handler);
                surface = new Surface(texture);
            } catch (Throwable error) { failure.set(error); }
            finally { ready.countDown(); }
        });
        if (!ready.await(10, TimeUnit.SECONDS)) { close(); throw new IllegalStateException("EGL setup timeout"); }
        if (failure.get() != null) { close(); throw new IllegalStateException("EGL setup", failure.get()); }
    }
    Surface surface() { return surface; }
    private static void require(boolean ok, String operation) {
        if (!ok) throw new IllegalStateException(operation + " EGL error " + EGL14.eglGetError());
    }
    @Override public void close() throws InterruptedException {
        CountDownLatch done = new CountDownLatch(1);
        handler.post(() -> {
            try {
                if (surface != null) surface.release();
                if (texture != null) texture.release();
                if (display != EGL14.EGL_NO_DISPLAY) {
                    EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT);
                    if (pbuffer != EGL14.EGL_NO_SURFACE) EGL14.eglDestroySurface(display, pbuffer);
                    if (context != EGL14.EGL_NO_CONTEXT) EGL14.eglDestroyContext(display, context);
                    EGL14.eglTerminate(display);
                }
            } finally { done.countDown(); }
        });
        done.await(5, TimeUnit.SECONDS); thread.quitSafely(); thread.join(5000);
    }
}
