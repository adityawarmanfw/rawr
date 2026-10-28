package com.rawr.camera.integration

import android.annotation.SuppressLint
import android.content.Context
import android.hardware.camera2.CameraAccessException
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CaptureFailure
import android.hardware.camera2.CaptureRequest
import android.hardware.camera2.TotalCaptureResult
import android.hardware.camera2.params.OutputConfiguration
import android.hardware.camera2.params.SessionConfiguration
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import android.util.Rational
import android.view.Surface
import androidx.annotation.Keep
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executor
import java.util.concurrent.TimeUnit

private const val TAG = "RawrJavaCamera"

// camera_status_t values the native side expects.
private const val OK = 0
private const val ERROR_UNKNOWN = -10000
private const val ERROR_INVALID_PARAMETER = -10001
private const val ERROR_CAMERA_DISCONNECTED = -10002
private const val ERROR_CAMERA_DEVICE = -10005
private const val ERROR_CAMERA_SERVICE = -10006
private const val ERROR_SESSION_CLOSED = -10007
private const val ERROR_STREAM_CONFIGURE_FAIL = -10009
private const val ERROR_CAMERA_IN_USE = -10010
private const val ERROR_MAX_CAMERA_IN_USE = -10011
private const val ERROR_CAMERA_DISABLED = -10012
private const val ERROR_PERMISSION_DENIED = -10013

private const val OPEN_TIMEOUT_S = 5L
private const val CONFIGURE_TIMEOUT_S = 10L

/**
 * The Java Camera2 backend of the native camera. Native code owns the readers and builds every request as data;
 * this class opens the camera, configures sessions (operating mode, stream use cases) and turns packed requests
 * into CaptureRequests. Calls come from native threads and block until Camera2 answers.
 */
@Keep
class JavaCameraBackend(context: Context) {
    private val manager = context.applicationContext.getSystemService(CameraManager::class.java)

    @Keep
    @SuppressLint("MissingPermission")
    fun open(cameraId: String, deviceHandle: Long): JavaCameraDevice {
        val device = JavaCameraDevice(cameraId, deviceHandle, characteristicsOf(cameraId))
        device.open(manager)
        return device
    }

    private fun characteristicsOf(cameraId: String): CameraCharacteristics? =
        runCatching { manager.getCameraCharacteristics(cameraId) }.getOrNull()

    companion object {
        @JvmStatic external fun nativeOnDeviceError(device: Long, error: Int, disconnected: Boolean)
        @JvmStatic external fun nativeOnSessionClosed(session: Long)
        @JvmStatic external fun nativeOnCaptureStarted(session: Long, requestId: Long, timestamp: Long)
        @JvmStatic external fun nativeOnCaptureCompleted(session: Long, requestId: Long, result: Any)
        @JvmStatic external fun nativeOnCaptureFailed(
            session: Long, requestId: Long, frameNumber: Long, reason: Int, sequenceId: Int, bufferLost: Boolean,
        )
        @JvmStatic external fun nativeOnSequenceEnded(session: Long, sequenceId: Int, aborted: Boolean)
    }
}

/** One open camera with its own callback thread. */
@Keep
class JavaCameraDevice internal constructor(
    private val cameraId: String,
    private val handle: Long,
    characteristics: CameraCharacteristics?,
) {
    private val thread = HandlerThread("rawr-camera2-$cameraId").also { it.start() }
    internal val handler = Handler(thread.looper)
    private val executor = Executor { handler.post(it) }
    internal val keys = RequestKeys(characteristics)
    @Volatile internal var closed = false
    @Volatile private var device: CameraDevice? = null
    @Volatile private var status = OK
    @Volatile private var error = ""
    // Released by a device error, so a native thread waiting on Camera2 never outlives the device.
    @Volatile private var pending: CountDownLatch? = null
    private var session: JavaCameraSession? = null

    @Keep fun status(): Int = status
    @Keep fun error(): String = error

    @SuppressLint("MissingPermission")
    internal fun open(manager: CameraManager) {
        val opened = CountDownLatch(1)
        pending = opened
        try {
            manager.openCamera(cameraId, executor, object : CameraDevice.StateCallback() {
                override fun onOpened(camera: CameraDevice) {
                    if (closed) camera.close() else device = camera
                    opened.countDown()
                }

                override fun onDisconnected(camera: CameraDevice) = failed(camera, 0, true)

                override fun onError(camera: CameraDevice, code: Int) = failed(camera, code, false)

                override fun onClosed(camera: CameraDevice) {
                    thread.quitSafely()
                }

                private fun failed(camera: CameraDevice, code: Int, disconnected: Boolean) {
                    val wasOpen = device != null
                    if (!wasOpen) {
                        status = openError(code, disconnected)
                        error = if (disconnected) "disconnected" else "device_error_$code"
                        camera.close()
                    }
                    pending?.countDown()
                    if (wasOpen && !closed) JavaCameraBackend.nativeOnDeviceError(handle, code, disconnected)
                }
            })
        } catch (e: CameraAccessException) {
            fail(accessError(e.reason), "access_${e.reason}")
        } catch (e: SecurityException) {
            fail(ERROR_PERMISSION_DENIED, "permission")
        } catch (e: IllegalArgumentException) {
            fail(ERROR_INVALID_PARAMETER, "unknown_camera")
        }
        if (status == OK && !opened.await(OPEN_TIMEOUT_S, TimeUnit.SECONDS)) fail(ERROR_CAMERA_DEVICE, "open_timeout")
        if (status == OK && device == null) fail(ERROR_UNKNOWN, "not_opened")
        if (status != OK) shutDown()
    }

    @Keep
    fun configure(
        surfaces: Array<Surface>,
        physicalIds: Array<String?>,
        useCases: LongArray,
        opMode: Int,
        sessionParameters: ByteArray?,
        sessionHandle: Long,
    ): JavaCameraSession? {
        val camera = device ?: return fail(ERROR_CAMERA_DISCONNECTED, "no_device").let { null }
        status = OK
        error = ""
        return try {
            val outputs = surfaces.indices.map { i ->
                OutputConfiguration(surfaces[i]).apply {
                    physicalIds[i]?.let { setPhysicalCameraId(it) }
                    if (useCases[i] != 0L) streamUseCase = useCases[i]
                }
            }
            val physical = physicalIds.firstOrNull()
            val created = JavaCameraSession(this, camera, sessionHandle, surfaces.toList(), physical)
            val configured = CountDownLatch(1)
            pending = configured
            val config = SessionConfiguration(opMode, outputs, executor, created.stateCallback(configured))
            sessionParameters?.let { config.sessionParameters = created.build(it, forSession = true) }
            camera.createCaptureSession(config)
            if (!configured.await(CONFIGURE_TIMEOUT_S, TimeUnit.SECONDS)) {
                created.abandon()
                return fail(ERROR_STREAM_CONFIGURE_FAIL, "configure_timeout").let { null }
            }
            if (!created.configured) return fail(ERROR_STREAM_CONFIGURE_FAIL, created.error.ifEmpty { "configure_failed" }).let { null }
            session = created
            created
        } catch (e: CameraAccessException) {
            fail(accessError(e.reason), "access_${e.reason}: ${e.message}").let { null }
        } catch (e: RuntimeException) {
            Log.w(TAG, "configure camera=$cameraId", e)
            fail(ERROR_INVALID_PARAMETER, "${e.javaClass.simpleName}: ${e.message}").let { null }
        }
    }

    /** Closes the camera; returns once no callback for it is running or will reach native code. */
    @Keep
    fun close() {
        closed = true
        pending?.countDown()
        runCatching { device?.close() }
        device = null
        val drained = CountDownLatch(1)
        if (handler.post { drained.countDown() }) drained.await(2, TimeUnit.SECONDS)
    }

    private fun shutDown() {
        closed = true
        thread.quitSafely()
    }

    private fun fail(code: Int, reason: String) {
        status = code
        error = reason
        Log.w(TAG, "camera=$cameraId status=$code $reason")
    }

    private fun openError(code: Int, disconnected: Boolean) = when {
        disconnected -> ERROR_CAMERA_DISCONNECTED
        code == CameraDevice.StateCallback.ERROR_CAMERA_IN_USE -> ERROR_CAMERA_IN_USE
        code == CameraDevice.StateCallback.ERROR_MAX_CAMERAS_IN_USE -> ERROR_MAX_CAMERA_IN_USE
        code == CameraDevice.StateCallback.ERROR_CAMERA_DISABLED -> ERROR_CAMERA_DISABLED
        code == CameraDevice.StateCallback.ERROR_CAMERA_SERVICE -> ERROR_CAMERA_SERVICE
        else -> ERROR_CAMERA_DEVICE
    }

    private fun accessError(reason: Int) = when (reason) {
        CameraAccessException.CAMERA_IN_USE -> ERROR_CAMERA_IN_USE
        CameraAccessException.MAX_CAMERAS_IN_USE -> ERROR_MAX_CAMERA_IN_USE
        CameraAccessException.CAMERA_DISABLED -> ERROR_CAMERA_DISABLED
        CameraAccessException.CAMERA_DISCONNECTED -> ERROR_CAMERA_DISCONNECTED
        else -> ERROR_CAMERA_DEVICE
    }
}

/** One configured session. Requests arrive packed by native code (see JavaCameraDriver.cpp). */
@Keep
class JavaCameraSession internal constructor(
    private val owner: JavaCameraDevice,
    private val camera: CameraDevice,
    private val handle: Long,
    private val surfaces: List<Surface>,
    private val physicalId: String?,
) {
    @Volatile internal var configured = false
    @Volatile internal var error = ""
    @Volatile private var session: CameraCaptureSession? = null
    @Volatile private var abandoned = false

    @Keep fun error(): String = error

    internal fun stateCallback(done: CountDownLatch) = object : CameraCaptureSession.StateCallback() {
        override fun onConfigured(s: CameraCaptureSession) {
            if (abandoned) {
                s.close()
            } else {
                session = s
                configured = true
            }
            done.countDown()
        }

        override fun onConfigureFailed(s: CameraCaptureSession) {
            error = "configure_failed"
            done.countDown()
        }

        override fun onClosed(s: CameraCaptureSession) {
            // Only a session native code accepted has a context to release.
            if (configured && !abandoned && !owner.closed) JavaCameraBackend.nativeOnSessionClosed(handle)
        }
    }

    internal fun abandon() {
        abandoned = true
        session?.close()
    }

    private val callback = object : CameraCaptureSession.CaptureCallback() {
        override fun onCaptureStarted(s: CameraCaptureSession, request: CaptureRequest, timestamp: Long, frameNumber: Long) {
            if (!owner.closed) JavaCameraBackend.nativeOnCaptureStarted(handle, idOf(request), timestamp)
        }

        override fun onCaptureCompleted(s: CameraCaptureSession, request: CaptureRequest, result: TotalCaptureResult) {
            if (owner.closed) return
            // A RAW stream from a physical camera pairs with that camera's own result.
            val chosen = physicalId?.let { result.physicalCameraTotalResults[it] } ?: result
            JavaCameraBackend.nativeOnCaptureCompleted(handle, idOf(request), chosen)
        }

        override fun onCaptureFailed(s: CameraCaptureSession, request: CaptureRequest, failure: CaptureFailure) {
            if (!owner.closed) {
                JavaCameraBackend.nativeOnCaptureFailed(
                    handle, idOf(request), failure.frameNumber, failure.reason, failure.sequenceId, false,
                )
            }
        }

        override fun onCaptureBufferLost(s: CameraCaptureSession, request: CaptureRequest, target: Surface, frameNumber: Long) {
            if (!owner.closed) JavaCameraBackend.nativeOnCaptureFailed(handle, idOf(request), frameNumber, 0, -1, true)
        }

        override fun onCaptureSequenceCompleted(s: CameraCaptureSession, sequenceId: Int, frameNumber: Long) {
            if (!owner.closed) JavaCameraBackend.nativeOnSequenceEnded(handle, sequenceId, false)
        }

        override fun onCaptureSequenceAborted(s: CameraCaptureSession, sequenceId: Int) {
            if (!owner.closed) JavaCameraBackend.nativeOnSequenceEnded(handle, sequenceId, true)
        }
    }

    @Keep
    fun setRepeating(packed: ByteArray): Int = submit { s -> s.setRepeatingRequest(build(packed), callback, owner.handler) }

    @Keep
    fun capture(packed: Array<ByteArray>): Int = submit { s ->
        val requests = packed.map { build(it) }
        if (requests.size == 1) s.capture(requests[0], callback, owner.handler)
        else s.captureBurst(requests, callback, owner.handler)
    }

    @Keep
    fun stopRepeating(): Int = submit { s ->
        s.stopRepeating()
        0
    }

    @Keep
    fun close() {
        session?.close()
    }

    private inline fun submit(block: (CameraCaptureSession) -> Int): Int {
        val s = session ?: return ERROR_SESSION_CLOSED
        return try {
            block(s)
        } catch (e: CameraAccessException) {
            error = "access_${e.reason}: ${e.message}"
            ERROR_CAMERA_DEVICE
        } catch (e: IllegalStateException) {
            error = "state: ${e.message}"
            ERROR_SESSION_CLOSED
        } catch (e: IllegalArgumentException) {
            error = "argument: ${e.message}"
            ERROR_INVALID_PARAMETER
        }
    }

    /** Builds a CaptureRequest from the native packing: template, id, targets, then tag/type/count/name/values. */
    internal fun build(packed: ByteArray, forSession: Boolean = false): CaptureRequest {
        val b = ByteBuffer.wrap(packed).order(ByteOrder.LITTLE_ENDIAN)
        val template = b.int
        val id = b.long
        val builder = camera.createCaptureRequest(template)
        repeat(b.int) {
            val index = b.int
            if (!forSession) builder.addTarget(surfaces[index])
        }
        repeat(b.int) {
            val tag = b.int
            val type = b.int
            val count = b.int
            val name = ByteArray(b.int).also { b.get(it) }.toString(Charsets.UTF_8)
            val data = ByteArray(b.int).also { b.get(it) }
            val keyName = name.ifEmpty { owner.keys.nameOf(tag) }
                ?: if (count == 0) return@repeat else throw IllegalArgumentException("no Camera2 key for tag 0x%x".format(tag))
            owner.keys.set(builder, keyName, type, count, data)
        }
        builder.setTag(id)
        return builder.build()
    }

    private fun idOf(request: CaptureRequest): Long = request.tag as? Long ?: 0L
}

/** Camera2 keys of one camera by native tag, and values set with the primitive type of the tag. */
internal class RequestKeys(characteristics: CameraCharacteristics?) {
    private val names: Map<Int, String> = buildMap {
        // Every framework key, listed by this camera or not: the NDK sets unlisted ones too (distortion mode).
        for (key in frameworkKeys) tagOf(key)?.let { put(it, key.name) }
        val keys = characteristics?.let {
            it.availableCaptureRequestKeys + it.availableSessionKeys.orEmpty() +
                it.availablePhysicalCameraRequestKeys.orEmpty()
        }.orEmpty()
        for (key in keys) tagOf(key)?.let { put(it, key.name) }
    }
    private val cache = HashMap<Pair<String, Int>, CaptureRequest.Key<Any>>()

    fun nameOf(tag: Int): String? = names[tag]

    @Suppress("UNCHECKED_CAST")
    fun set(builder: CaptureRequest.Builder, name: String, type: Int, count: Int, data: ByteArray) {
        val key = cache.getOrPut(name to type) { CaptureRequest.Key(name, classOf(type)) as CaptureRequest.Key<Any> }
        builder.set(key, if (count == 0) null else valueOf(type, count, data))
    }

    private fun classOf(type: Int): Class<*> = when (type) {
        0 -> ByteArray::class.java
        1 -> IntArray::class.java
        2 -> FloatArray::class.java
        3 -> LongArray::class.java
        4 -> DoubleArray::class.java
        5 -> Array<Rational>::class.java
        else -> throw IllegalArgumentException("metadata type $type")
    }

    private fun valueOf(type: Int, count: Int, data: ByteArray): Any {
        val b = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN)
        return when (type) {
            0 -> data.copyOf(count)
            1 -> IntArray(count) { b.int }
            2 -> FloatArray(count) { b.float }
            3 -> LongArray(count) { b.long }
            4 -> DoubleArray(count) { b.double }
            5 -> Array(count) { Rational(b.int, b.int) }
            else -> throw IllegalArgumentException("metadata type $type")
        }
    }

    private companion object {
        val frameworkKeys: List<CaptureRequest.Key<*>> by lazy {
            CaptureRequest::class.java.fields.mapNotNull { field ->
                if (field.type == CaptureRequest.Key::class.java) field.get(null) as? CaptureRequest.Key<*> else null
            }
        }

        // Same hidden lookup as CameraTagResolver: the key's tag for this camera's vendor tag provider.
        fun tagOf(key: CaptureRequest.Key<*>): Int? = runCatching {
            val native = CaptureRequest.Key::class.java.getDeclaredMethod("getNativeKey").invoke(key)
            native!!.javaClass.getDeclaredMethod("getTag").invoke(native) as Int
        }.getOrNull()
    }
}
