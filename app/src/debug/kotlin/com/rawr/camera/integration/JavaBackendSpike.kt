package com.rawr.camera.integration

import android.annotation.SuppressLint
import android.content.Context
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CaptureRequest
import android.hardware.camera2.TotalCaptureResult
import android.hardware.camera2.params.OutputConfiguration
import android.hardware.camera2.params.SessionConfiguration
import android.os.Handler
import android.os.HandlerThread
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executor
import java.util.concurrent.TimeUnit

/**
 * Debug spike: a Java Camera2 session whose outputs are native AImageReader windows and whose capture
 * results are read through ACameraMetadata_fromCameraMetadata.
 */
class JavaBackendSpike(private val context: Context) {
    class Options(
        val cameraId: String,
        val width: Int,
        val height: Int,
        val format: Int,
        val companionWidth: Int,
        val companionHeight: Int,
        val streamUseCase: Long,
        val opMode: Int,
        val template: Int,
        val seconds: Int,
        val physicalId: String?,
    )

    private external fun nativeCreate(w: Int, h: Int, format: Int, compW: Int, compH: Int): Long
    private external fun nativeSurface(handle: Long, companion: Boolean): android.view.Surface?
    private external fun nativeOnResult(handle: Long, result: Any)
    private external fun nativeReport(handle: Long): String
    private external fun nativeDestroy(handle: Long)

    @SuppressLint("MissingPermission")
    fun run(o: Options, log: (String) -> Unit): String {
        val thread = HandlerThread("java-backend-spike").also { it.start() }
        val handler = Handler(thread.looper)
        val executor = Executor { handler.post(it) }
        val manager = context.getSystemService(CameraManager::class.java)
        val handle = nativeCreate(o.width, o.height, o.format, o.companionWidth, o.companionHeight)
        if (handle == 0L) return "FAIL native readers"
        var device: CameraDevice? = null
        var session: CameraCaptureSession? = null
        var failure: String? = null
        try {
            val opened = CountDownLatch(1)
            manager.openCamera(o.cameraId, executor, object : CameraDevice.StateCallback() {
                override fun onOpened(camera: CameraDevice) { device = camera; opened.countDown() }
                override fun onDisconnected(camera: CameraDevice) { failure = "disconnected"; opened.countDown() }
                override fun onError(camera: CameraDevice, error: Int) { failure = "device error $error"; opened.countDown() }
            })
            if (!opened.await(10, TimeUnit.SECONDS)) return "FAIL open timeout"
            failure?.let { return "FAIL $it" }
            val camera = device!!
            log("opened ${o.cameraId}")
            val rawSurface = nativeSurface(handle, false) ?: return "FAIL raw surface"
            val outputs = mutableListOf(OutputConfiguration(rawSurface).also { out ->
                if (o.streamUseCase != 0L) out.streamUseCase = o.streamUseCase
                o.physicalId?.let { out.setPhysicalCameraId(it) }
            })
            val companion = if (o.companionWidth > 0) nativeSurface(handle, true) else null
            if (companion != null) outputs += OutputConfiguration(companion)
            val configured = CountDownLatch(1)
            val config = SessionConfiguration(o.opMode, outputs, executor, object : CameraCaptureSession.StateCallback() {
                override fun onConfigured(s: CameraCaptureSession) { session = s; configured.countDown() }
                override fun onConfigureFailed(s: CameraCaptureSession) { failure = "configure failed"; configured.countDown() }
            })
            camera.createCaptureSession(config)
            if (!configured.await(10, TimeUnit.SECONDS)) return "FAIL configure timeout"
            failure?.let { return "FAIL $it" }
            log("configured")
            val builder = camera.createCaptureRequest(o.template)
            builder.addTarget(rawSurface)
            if (companion != null) builder.addTarget(companion)
            session!!.setRepeatingRequest(builder.build(), object : CameraCaptureSession.CaptureCallback() {
                override fun onCaptureCompleted(s: CameraCaptureSession, r: CaptureRequest, result: TotalCaptureResult) {
                    nativeOnResult(handle, result)
                }
            }, handler)
            Thread.sleep(o.seconds * 1000L)
            return nativeReport(handle)
        } catch (t: Throwable) {
            return "FAIL ${t.javaClass.simpleName}: ${t.message}"
        } finally {
            runCatching { session?.close() }
            runCatching { device?.close() }
            Thread.sleep(300)
            nativeDestroy(handle)
            thread.quitSafely()
        }
    }

    companion object {
        init { System.loadLibrary("rawrcam_native") }
    }
}
