package com.rawr.camera.integration

import android.content.Context
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CaptureRequest
import android.util.Log
import androidx.annotation.Keep

/** Process-local, camera-scoped fallback for Android 13/14's missing NDK name lookup. */
@Keep
class CameraTagResolver internal constructor(private val lookup: (String, String) -> Int) {
    constructor(context: Context) : this(Camera2TagLookup(context)::resolve)
    private val resolved = mutableMapOf<Pair<String, String>, Long>()

    // These Camera2 compatibility methods are @UnsupportedAppUsage in Android 13/14.
    // Keep reflection isolated and fail explicitly on firmware that denies access.
    @Synchronized
    fun resolve(cameraId: String, name: String): Long {
        val cacheKey = cameraId to name
        resolved[cacheKey]?.let { return it }
        return try {
            val tag = lookup(cameraId, name).toLong() and 0xffffffffL
            resolved[cacheKey] = tag
            tag
        } catch (e: Exception) {
            Log.w("RawrCameraKeys", "Cannot resolve camera=$cameraId key=$name through Camera2", e)
            -1L // Never cache failure: a later probe may succeed after camera service recovery.
        }
    }
}

private class Camera2TagLookup(context: Context) {
    private val manager = context.applicationContext.getSystemService(CameraManager::class.java)

    fun resolve(cameraId: String, name: String): Int {
        val characteristics = manager.getCameraCharacteristics(cameraId)
        val keys = characteristics.availableCaptureRequestKeys +
            characteristics.availableSessionKeys.orEmpty() +
            characteristics.availablePhysicalCameraRequestKeys.orEmpty()
        // Device keys carry the provider ID; never resolve against a global/default provider.
        val key = keys.firstOrNull { it.name == name }
        val nativeKey = if (key != null) {
            CaptureRequest.Key::class.java.getDeclaredMethod("getNativeKey").invoke(key)
        } else {
            // Some HALs accept vendor request tags omitted from availableRequestKeys.
            // A read resolves the name against this camera and caches the tag on the key,
            // even when the request-only tag has no value in static characteristics.
            // Unknown names throw here; never proceed to a provider-less lookup on failure.
            val probe = CameraCharacteristics.Key(name, ByteArray::class.java)
            characteristics.get(probe)
            CameraCharacteristics.Key::class.java.getDeclaredMethod("getNativeKey").invoke(probe)
        } ?: error("Camera2 returned no native key")
        return nativeKey.javaClass.getDeclaredMethod("getTag").invoke(nativeKey) as Int
    }
}
