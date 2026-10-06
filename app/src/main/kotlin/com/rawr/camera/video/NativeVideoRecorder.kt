package com.rawr.camera.video

import androidx.core.net.toUri
import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.media.MediaExtractor
import android.media.MediaFormat
import android.net.Uri
import android.os.Build
import android.os.SystemClock
import android.view.Surface
import com.rawr.camera.BuildConfig
import com.rawr.camera.integration.RawPreviewCoordinator
import com.rawr.camera.storage.CaptureFileNames
import com.rawr.camera.storage.VideoOutputStore
import org.json.JSONObject
import java.io.File

/** Android lifecycle and storage adapter for the C++ HEVC/AAC/MP4 recorder. */
class NativeVideoRecorder(private val context: Context, private val preview: RawPreviewCoordinator) : VideoRecording {
    companion object {
        /** Compact labels for the recording monitor's DROP cell. */
        private val DROP_REASON_LABELS = mapOf(
            "ingress" to "in", "noSlot" to "gpu", "encoderBusy" to "enc", "presentFail" to "pres",
            "pairer" to "pair", "stale" to "old", "submitFail" to "err", "geometry" to "size")

        /** The UI can enumerate VideoTimestampPolicy.entries and disable unsupported choices. */
        fun supportedTimestampPolicies(): Set<VideoTimestampPolicy> {
            val supportedIds = NativeAvEngine().nativeSupportedTimestampPolicies()
            return VideoTimestampPolicy.entries.filter { it.nativeId in supportedIds }.toSet()
        }

        /** Finalizes a playable MP4 at its original destination after interrupted publication. */
        fun recoverInterrupted(context: Context) {
            context.filesDir.listFiles { file ->
                file.name.startsWith("rawr_native_video_") && file.name.endsWith(".jsonl")
            }?.forEach { journal ->
                runCatching {
                    var first: String? = null
                    var last: String? = null
                    journal.bufferedReader().use { reader ->
                        while (true) {
                            val line = reader.readLine() ?: break
                            if (line.isBlank()) continue
                            if (first == null) first = line
                            last = line
                        }
                    }
                    if (first == null || last == null) return@runCatching
                    val lastEvent = JSONObject(last).optString("event")
                    if (lastEvent == "published" || lastEvent == "recovered") return@runCatching
                    val uri = JSONObject(first).getString("uri").toUri()
                    val extractor = MediaExtractor()
                    try {
                        extractor.setDataSource(context, uri, null)
                        var video = false
                        var audio = false
                        for (index in 0 until extractor.trackCount) {
                            val mime = extractor.getTrackFormat(index).getString(MediaFormat.KEY_MIME).orEmpty()
                            extractor.selectTrack(index)
                            val hasSample = extractor.sampleTime >= 0
                            extractor.unselectTrack(index)
                            if (hasSample && mime.startsWith("video/")) video = true
                            if (hasSample && mime.startsWith("audio/")) audio = true
                        }
                        if (!video || !audio) return@runCatching
                    } finally {
                        extractor.release()
                    }
                    VideoOutputStore(context).publish(uri)
                    journal.appendText(JSONObject().put("event", "recovered").toString() + "\n")
                }.onFailure { android.util.Log.w("RawrVideo", "Could not recover ${journal.name}", it) }
            }
        }
    }

    data class Settings(val fps: Int = 30, val bitrate: Int = 12_000_000, val intraSeconds: Int = 1,
                        val width: Int = 1920, val height: Int = 1080, val mode: String = "1080p",
                        val deviceRotationDegrees: Int = 0,
                        /** -1 lets the codec choose; 0=CQ, 1=VBR, 2=CBR (default). */
                        val bitrateMode: Int = 2,
                        /** -1 lets the codec choose; 0 disables B frames. */
                        val maxBFrames: Int = -1,
                        /** 1=mono, 2=stereo (default). Stereo falls back to mono at 128k if unsupported. */
                        val audioChannels: Int = 2,
                        /** AAC bitrate. Stereo default 192k; mono fallback steps down to 128k. */
                        val audioBitrate: Int = 192_000,
                        /** 8 = HEVC Main, 10 = HEVC Main10 (default). */
                        val bitDepth: Int = 10,
                        val renderProfile: String = "RAWR NTRL",
                        val renderProfileId: Int = 0,
                        val logProfile: com.rawr.camera.settings.model.VideoLogProfile? = null,
                        val timestampPolicy: VideoTimestampPolicy = VideoTimestampPolicy.REALTIME,
                        val saveLocationId: String = "storage.dcim_camera")

    private val engine = NativeAvEngine()
    private val outputStore = VideoOutputStore(context)
    private var handle = 0L
    private var surface: Surface? = null
    private var uri: Uri? = null
    private var journal: File? = null
    private var settings = Settings()
    override val isRecording: Boolean get() = handle != 0L
    override val outputUri: Uri? get() = uri

    @Synchronized fun start(request: Settings = Settings()) {
        check(handle == 0L) { "Video recording already active" }
        check(context.checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED) {
            "Microphone permission required"
        }
        require(request.fps == 24 || request.fps == 30)
        require(request.bitrate in VideoEncoderLimits.MIN_BITRATE_MBPS * 1_000_000..
            VideoEncoderLimits.MAX_BITRATE_MBPS * 1_000_000)
        require(request.intraSeconds in 1..10)
        require(request.bitrateMode in -1..2)
        require(request.maxBFrames in -1..4)
        require(request.audioChannels == 1 || request.audioChannels == 2)
        require(request.audioBitrate in 64_000..512_000)
        require(request.bitDepth == 8 || request.bitDepth == 10)
        require(request.logProfile == null || request.bitDepth == 10) { "LOG requires 10-bit recording" }
        require(request.timestampPolicy in supportedTimestampPolicies()) {
            "Timestamp policy ${request.timestampPolicy.label} is unavailable on this recording path"
        }
        require(request.width > 0 && request.height > 0 && request.width % 2 == 0 && request.height % 2 == 0)
        settings = request
        val rotation = preview.videoRotationDegrees(request.deviceRotationDegrees)
        check(rotation >= 0) { "Camera orientation is not ready for recording" }
        val capturedAt = System.currentTimeMillis()
        val displayName = CaptureFileNames.baseName(capturedAt) + ".mp4"
        uri = null
        journal = null
        try {
            check(preview.beginVideoFps(request.fps)) { "RAW camera could not lock ${request.fps} fps" }
            uri = outputStore.create(request.saveLocationId, displayName, capturedAt)
            // The detached descriptor is closed by C++, including nativeStart failure paths.
            // Read+write provides the seekable descriptor needed by the muxer and metadata patcher.
            // A provider that cannot supply one fails here; never redirect to another folder.
            val fd = requireNotNull(context.contentResolver.openFileDescriptor(requireNotNull(uri), "rw"))
                .use { it.detachFd() }
            handle = engine.nativeStart(fd, request.width, request.height, request.fps,
                request.bitrate, request.intraSeconds, rotation, request.bitrateMode, request.maxBFrames,
                request.audioChannels, request.audioBitrate, request.bitDepth,
                request.timestampPolicy.nativeId, Build.MANUFACTURER.orEmpty(), Build.MODEL.orEmpty(),
                "RAWR ${BuildConfig.VERSION_NAME}", request.renderProfile,
                request.logProfile?.gamut?.label ?: "Rec.709 / sRGB",
                request.logProfile?.transfer?.label ?: if (request.renderProfileId == 3) "BT.709" else "sRGB",
                request.logProfile != null)
            check(handle != 0L) { "Native recorder did not start" }
            surface = engine.nativeSurface(handle)
            check(preview.setVideoSurface(surface, request.width, request.height, request.bitDepth)) {
                "GPU encoder surface unavailable at ${request.width}×${request.height}"
            }
            journal = File(context.filesDir, "rawr_native_video_${System.currentTimeMillis()}.jsonl")
            append(JSONObject().put("event", "start").put("uri", uri.toString())
                .put("displayName", displayName).put("saveLocationId", request.saveLocationId)
                .put("bootNs", SystemClock.elapsedRealtimeNanos()).put("camera", preview.cameraControlSnapshot())
                .put("fps", request.fps).put("bitrate", request.bitrate)
                .put("mode", request.mode).put("width", request.width).put("height", request.height)
                .put("intraSeconds", request.intraSeconds).put("rotationDegrees", rotation)
                .put("bitrateMode", request.bitrateMode).put("maxBFrames", request.maxBFrames)
                .put("audioChannels", request.audioChannels).put("audioBitrate", request.audioBitrate)
                .put("bitDepth", request.bitDepth)
                .put("renderProfile", request.renderProfile).put("renderProfileId", request.renderProfileId)
                .put("logProfile", request.logProfile?.name)
                .put("gamut", request.logProfile?.gamut?.name ?: "SRgbRec709")
                .put("transfer", request.logProfile?.transfer?.name ?: if (request.renderProfileId == 3) "Rec709" else "SRgb")
                .put("timestampPolicy", request.timestampPolicy.name)
                .put("processing", preview.videoImageSettingsSnapshot()?.toJson()))
        } catch (error: Throwable) {
            runCatching { preview.setVideoSurface(null) }
            runCatching { preview.endVideoFps() }
            surface?.release()
            surface = null
            if (handle != 0L) {
                runCatching { engine.nativeStop(handle) }
                handle = 0L
            }
            uri?.let { runCatching { outputStore.delete(it) } }
            uri = null
            throw error
        }
    }

    @Synchronized override fun stats(): JSONObject {
        val gpu = runCatching { JSONObject(preview.videoStats()) }.getOrDefault(JSONObject())
        val native = if (handle != 0L) JSONObject(engine.nativeStats(handle)) else JSONObject()
        val keys = native.keys()
        while (keys.hasNext()) {
            val key = keys.next()
            gpu.put(key, native.get(key))
        }
        // "dropped" counts camera frames lost on the way to the encoder. A slow
        // sensor cadence is a target shortfall instead; encodedFrameGaps stays
        // in the journal as the file-side cross-check.
        gpu.optJSONObject("dropReasons")?.let { r ->
            val top = r.keys().asSequence().maxByOrNull { r.optLong(it) }
            if (top != null && r.optLong(top) > 0) gpu.put("dropReason", DROP_REASON_LABELS[top] ?: top)
        }
        gpu.optJSONObject("stageMs")?.let { stage ->
            stage.optDouble("total").takeIf { it > 0 }?.let { gpu.put("videoGpuMs", it) }
            stage.optDouble("totalPeak").takeIf { it > 0 }?.let { gpu.put("videoGpuPeakMs", it) }
        }
        gpu.put("mode", settings.mode)
        gpu.put("timestampPolicy", settings.timestampPolicy.name)
        return gpu
    }

    @Synchronized override fun journalSnapshot() {
        if (isRecording) append(JSONObject().put("event", "sample")
            .put("bootNs", SystemClock.elapsedRealtimeNanos()).put("stats", stats())
            .put("camera", preview.cameraControlSnapshot())
            .put("processing", preview.videoImageSettingsSnapshot()?.toJson()))
    }

    @Synchronized override fun close() {
        val active = handle
        if (active == 0L) return
        var failure: Throwable? = null
        try {
            val detached = preview.setVideoSurface(null)
            val result = JSONObject(engine.nativeStop(active))
            handle = 0L
            append(JSONObject().put("event", "stop").put("result", result))
            check(detached) { "Could not detach video surface" }
            check(result.optBoolean("ok")) { "Native recording failed: ${result.optJSONObject("stats")?.optString("failure")}" }
            uri?.let(outputStore::publish)
            append(JSONObject().put("event", "published"))
        } catch (error: Throwable) {
            failure = error
            append(JSONObject().put("event", "error").put("error", error.toString()))
        } finally {
            if (handle != 0L) {
                runCatching { engine.nativeStop(handle) }
                handle = 0L
            }
            runCatching { preview.endVideoFps() }
            surface?.release()
            surface = null
        }
        if (failure != null) throw requireNotNull(failure)
    }

    private fun append(event: JSONObject) { journal?.appendText(event.toString() + "\n") }
}
