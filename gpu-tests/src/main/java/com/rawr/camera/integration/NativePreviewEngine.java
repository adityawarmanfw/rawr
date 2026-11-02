package com.rawr.camera.integration;

import android.content.res.AssetManager;
import android.view.Surface;

/** Test-only ABI adapter; every operation uses the production native engine. */
public final class NativePreviewEngine {
    static { System.loadLibrary("rawrcam_native"); }
    public native long create(String filesDir, String libraryDir, String driverPath);
    public native void destroy(long handle);
    public native boolean setSurface(long handle, Surface surface, int rotation);
    public native void setAssetManager(long handle, AssetManager assets);
    public native void setPreferredCameraId(long handle, String cameraId);
    public native String builtInCameraProfile();
    public native boolean setCameraProfile(long handle, String json);
    public native void setPersistentDiagnosticsEnabled(long handle, boolean enabled);
    public native void setPreviewForeground(long handle, boolean foreground);
    public native void setUiPresentationActive(long handle, boolean active);
    public native void setInitialConfigReady(long handle);
    public native void tickCameraSession(long handle);
    public native void setViewfinderDivisor(long handle, int divisor);
    public native void setExperimentalZeroCopy(long handle, boolean enabled);
    public native void setColorRenderProfile(long handle, int profile, String importedLut);
    public native void setScopePresentationState(long handle, int[] types, int[] modes, int[] rotations, float[] rects);
    public native String cameraControlSnapshot(long handle);
}
