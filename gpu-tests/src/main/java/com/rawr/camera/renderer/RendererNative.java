package com.rawr.camera.renderer;

import android.content.res.AssetManager;

/** Test-only JNI ABI adapter for the actual app renderer library. */
public final class RendererNative {
    static { System.loadLibrary("rawrcam_native"); }
    public native long open(String path, String files, String library, AssetManager assets);
    public native String metadata(long id);
    public native int[] render(long id, String recipe, int width, int height, int fd,
                               int[] crop, int fullWidth, int fullHeight);
    public native void close(long id);
}
