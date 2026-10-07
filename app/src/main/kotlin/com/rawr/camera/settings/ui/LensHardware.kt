package com.rawr.camera.settings.ui

import android.content.Context
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraManager
import android.os.Build
import androidx.compose.runtime.staticCompositionLocalOf
import com.rawr.camera.integration.CameraInventory
import com.rawr.camera.integration.CameraInventorySource
import com.rawr.camera.integration.CameraKeyCatalog
import com.rawr.camera.integration.DeviceLensDefaults
import com.rawr.camera.settings.model.LensProfile
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext

/** What the lens settings need from the device: built-in lenses and camera/key probes. */
interface LensHardware {
    val deviceDefaults: List<LensProfile>

    suspend fun cameras(): CameraInventory

    suspend fun keys(cameraId: String, physicalCameraId: String): CameraKeyCatalog

    /** True when any camera advertises a Camera2 shutter/ISO priority mode (API 36+). */
    suspend fun aePrioritySupported(): Boolean

    /** No device access (previews, renderer, tests). */
    object None : LensHardware {
        override val deviceDefaults: List<LensProfile> = emptyList()

        override suspend fun aePrioritySupported() = true

        override suspend fun cameras() = CameraInventory(emptyList(), "Camera probing is unavailable here")

        override suspend fun keys(cameraId: String, physicalCameraId: String) =
            CameraKeyCatalog(emptyList(), "Key probing is unavailable here")
    }
}

/** Probes once per settings screen lifetime; failed key probes are retried on the next request. */
class AndroidLensHardware(context: Context) : LensHardware {
    private val source = CameraInventorySource(context)
    private val mutex = Mutex()
    private var inventory: CameraInventory? = null
    private val keyCatalogs = mutableMapOf<Pair<String, String>, CameraKeyCatalog>()

    override val deviceDefaults: List<LensProfile> get() = DeviceLensDefaults.lenses

    override suspend fun cameras(): CameraInventory = mutex.withLock {
        inventory ?: withContext(Dispatchers.IO) { source.cameras() }.also { inventory = it }
    }

    private val cameraManager = context.getSystemService(CameraManager::class.java)
    private var aePriority: Boolean? = null

    override suspend fun aePrioritySupported(): Boolean = mutex.withLock {
        aePriority ?: withContext(Dispatchers.IO) { probeAePriority() }.also { aePriority = it }
    }

    private fun probeAePriority(): Boolean {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.BAKLAVA) return false
        return runCatching {
            cameraManager.cameraIdList.any { id ->
                val modes = cameraManager.getCameraCharacteristics(id)
                    .get(CameraCharacteristics.CONTROL_AE_AVAILABLE_PRIORITY_MODES)
                modes != null && modes.any { it != CameraCharacteristics.CONTROL_AE_PRIORITY_MODE_OFF }
            }
        }.getOrDefault(true)
    }

    override suspend fun keys(cameraId: String, physicalCameraId: String): CameraKeyCatalog = mutex.withLock {
        val id = cameraId to physicalCameraId
        keyCatalogs[id] ?: withContext(Dispatchers.IO) { source.keys(cameraId, physicalCameraId) }
            .also { if (it.error == null) keyCatalogs[id] = it }
    }
}

val LocalLensHardware = staticCompositionLocalOf<LensHardware> { LensHardware.None }
