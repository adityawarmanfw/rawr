package com.rawr.camera.renderer

import androidx.core.net.toUri
import android.content.Context
import android.net.Uri
import android.os.ParcelFileDescriptor
import com.rawr.camera.settings.model.SettingsValues
import com.rawr.camera.storage.StillOutputStore
import java.io.File
import java.io.FileOutputStream
import java.nio.file.Files
import java.nio.file.StandardCopyOption
import java.util.UUID
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import org.json.JSONObject

/** Coordinates renderer use cases under one native-operation lane. */
internal class RendererStore private constructor(private val context: Context) {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val lane = Mutex()
    private val repository = RendererJobRepository(context, scope)
    private val native = RendererNativeSession(context, repository::source)
    private val assets = RendererAssets(context, repository::directory)
    private val output = RendererOutputPublisher(context)
    @Volatile var visible = false
    private val exports = RendererExportCoordinator(
        context, scope, lane, repository, native, assets, output, { visible }, ::removeUnlocked)
    private val previews = RendererPreviewBackend(native, assets, lane, scope, { exports.busy.value }, { visible })
    val jobs = repository.jobs
    val busy = exports.busy
    val completion = exports.completion
    val progress = exports.progress
    suspend fun awaitReady() = repository.awaitReady()
    fun refresh() = repository.refresh()
    fun save(job: RenderJob) = repository.save(job)
    fun current(id: String) = repository.current(id)
    fun updateDraft(id: String, values: SettingsValues) = repository.updateDraft(id, values)
    fun updateAdaptiveToneMode(id: String, mode: com.rawr.camera.settings.model.AutoKeyExposure) =
        repository.updateAdaptiveToneMode(id, mode)
    suspend fun metadata(job: RenderJob): String = withContext(Dispatchers.IO) {
        lane.withLock { native.metadata(job) }
    }
    suspend fun previewPixels(job: RenderJob, size: RendererImageSize, refined: Boolean, waveform: Boolean,
        crop: IntArray? = null) = previews.render(job, size, refined, waveform, crop)
    fun previewStage() = native.stage()
    fun previewBudget(): RendererPreviewBudget {
        val info = android.app.ActivityManager.MemoryInfo()
        context.getSystemService(android.app.ActivityManager::class.java).getMemoryInfo(info)
        val runtime = Runtime.getRuntime()
        return RendererPreviewBudget.admit(runtime.maxMemory() - runtime.totalMemory() + runtime.freeMemory(),
            (info.availMem - info.threshold).coerceAtLeast(0), info.lowMemory)
    }
    @Suppress("DEPRECATION", "OVERRIDE_DEPRECATION")
    fun observePreviewMemory(trim: () -> Unit): AutoCloseable {
        val callback = object : android.content.ComponentCallbacks2 {
            override fun onConfigurationChanged(config: android.content.res.Configuration) = Unit
            override fun onLowMemory() = trim()
            override fun onTrimMemory(level: Int) { if (level >= android.content.ComponentCallbacks2.TRIM_MEMORY_RUNNING_LOW) trim() }
        }
        context.registerComponentCallbacks(callback)
        return AutoCloseable { context.unregisterComponentCallbacks(callback) }
    }
    suspend fun closePreview() = previews.closePreview()
    fun releasePreview() = previews.releasePreview()
    fun cancel() = exports.cancel()
    fun cancelPreview() { if (!busy.value) native.cancelRefinement() }
    fun hasInterruptedExport() = exports.hasInterruptedExport()
    fun resumeInterrupted() = exports.resumeInterrupted()
    fun export(id: String, original: Boolean = false, resume: Boolean = false) = exports.export(id, original, resume)
    suspend fun import(uri: Uri, library: SettingsValues): RenderJob = withContext(Dispatchers.IO) {
        lane.withLock {
            val id = UUID.randomUUID().toString(); val d = repository.directory(id).apply { mkdirs() }
            try {
                val columns = arrayOf(android.provider.OpenableColumns.DISPLAY_NAME, android.provider.OpenableColumns.SIZE)
                val (name, size) = context.contentResolver.query(uri, columns, null, null, null)?.use {
                    if (it.moveToFirst()) (it.getString(0) ?: "Imported DNG") to (if (it.isNull(1)) -1L else it.getLong(1))
                    else "Imported DNG" to -1L
                } ?: ("Imported DNG" to -1L)
                val started = android.os.SystemClock.elapsedRealtime()
                // One storage reservation for the whole file: StorageManager
                // calls per 1 MB chunk cost seconds on a 200 MP DNG. Unknown or
                // overrunning sizes fall back to a check per 64 MB copied.
                if (size > 0) repository.checkImportSpace(size)
                val temp = File(d, "input.tmp")
                var copied = 0L
                var syncMs = 0L
                context.contentResolver.openInputStream(uri)!!.use { input -> FileOutputStream(temp).use { out ->
                    val buffer = ByteArray(1024 * 1024)
                    var unchecked = 0L
                    while (true) { val n = input.read(buffer); if (n < 0) break
                        copied += n
                        if (size <= 0 || copied > size) {
                            unchecked += n
                            if (unchecked >= IMPORT_CHECK_BYTES) { repository.checkImportSpace(unchecked); unchecked = 0L }
                        }
                        out.write(buffer, 0, n)
                    }
                    if (unchecked > 0) repository.checkImportSpace(unchecked)
                    val syncStarted = android.os.SystemClock.elapsedRealtime()
                    out.fd.sync()
                    syncMs = android.os.SystemClock.elapsedRealtime() - syncStarted
                } }
                Files.move(temp.toPath(), File(d, "input.dng").toPath(), StandardCopyOption.ATOMIC_MOVE)
                val copiedAt = android.os.SystemClock.elapsedRealtime()
                inspect(RenderJob(id, name), library).also(::save).also {
                    android.util.Log.i("RawrRenderer", "RENDERER_IMPORT bytes=$copied copyMs=${copiedAt - started} " +
                        "syncMs=$syncMs inspectMs=${android.os.SystemClock.elapsedRealtime() - copiedAt}")
                }
            } catch (error: Exception) {
                native.closeFor(id)
                d.deleteRecursively(); throw error
            }
        }
    }
    private fun inspect(job: RenderJob, library: SettingsValues): RenderJob {
        val data = native.inspect(job).split('\n', limit = 5)
        val w = data[0].toInt(); val h = data[1].toInt(); val rawr = data[3] == "1"
        val provenance = data.getOrNull(4)?.takeIf { it.isNotBlank() }?.let(::JSONObject)
        val ext = provenance?.optJSONObject("extensions")
        val embedded = ext?.optString("com.rawrcam.processing.recipe.v1")
        val original = job.original.ifBlank { embedded.orEmpty() }
            .ifBlank { RendererRecipe.encode(RendererRecipe.neutral(library)) }
        val resolved = ext?.optString("com.rawrcam.processing.resolved.v1")?.takeIf { it.isNotBlank() }?.let(::JSONObject)
        val frame = ext?.optString("com.rawrcam.processing.frame.v1")?.takeIf { it.isNotBlank() }?.let(::JSONObject)
        val frozen = JSONObject(original).apply {
            put("aePostGain", resolved?.optDouble("aePostGain", 1.0) ?: 1.0)
            resolved?.optJSONObject("tonemap")?.optJSONObject("adaptiveTone")?.let { put("adaptiveTone", it) }
            resolved?.optJSONObject("tonemap")?.optJSONObject("localTone")?.let { put("localTone", it) }
            put("timestampNs", frame?.optLong("timestampNs", 1_000_000_000) ?: 1_000_000_000)
            if (frame != null) put("resolvedFrame", frame)
            // The recipe's mode is the capture's; drafts re-encode autoKeyExposure from the editor settings.
            optString("autoKeyExposure").takeIf { it.isNotBlank() }?.let { put("capturedAutoKeyExposure", it) }
        }.toString().let(assets::resolve)
        RendererRecipe.decode(frozen, library) // Validate before the editor is composed.
        val draft = assets.resolve(RendererRecipe.startingDraft(frozen, library,
            captured = RendererRecipe.replaysRecipe(job.capture, embedded)))
        return job.copy(original = frozen, draft = draft, width = w, height = h, rawr = rawr,
            orientation = data[2].toInt(), megapixels = RenderResolution.defaultMegapixels(w, h, rawr))
    }
    suspend fun edit(job: RenderJob, library: SettingsValues): RenderJob = withContext(Dispatchers.IO) {
        lane.withLock {
            if (job.width > 0) return@withLock job
            check(File(context.filesDir, "still_jobs/${job.capture}.renderer-owner").exists() || StillOutputStore.claimForRenderer(context.filesDir, job.capture)) { "This capture is still processing" }
            try {
                save(job.copy(status = "Preparing RAW"))
                val file = repository.source(job)
                ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_CREATE or ParcelFileDescriptor.MODE_READ_WRITE or ParcelFileDescriptor.MODE_TRUNCATE).use { fd ->
                    native.materialize(job.capture, fd.fd)
                }
                inspect(job, library).also(::save).also {
                    // The editor now edits its own copy. Hand the capture back so camera
                    // recovery publishes its capture-time outputs under their own names.
                    StillOutputStore.markRendererEdited(context.filesDir, job.capture)
                    StillOutputStore.rendererRelease(context.filesDir, job.capture)
                }
            } catch (error: Exception) {
                runCatching { save(job.copy(status = "Failed", error = error.message.orEmpty())) }
                // The editor holds the capture only while preparing its copy.
                StillOutputStore.rendererRelease(context.filesDir, job.capture)
                throw error
            }
        }
    }
    suspend fun remove(job: RenderJob) = withContext(Dispatchers.IO) { lane.withLock { check(!busy.value); removeUnlocked(job) } }
    private fun removeUnlocked(job: RenderJob) {
        native.closeFor(job.id)
        // Editor jobs never own their capture: camera recovery publishes its capture-time
        // outputs. Only removing the unopened capture card itself discards the capture.
        val unopenedCapture = job.capture.isNotEmpty() &&
            runCatching { !File(repository.directory(job.id), "job.json").exists() }.getOrDefault(false)
        if (unopenedCapture) {
            check(File(context.filesDir, "still_jobs/${job.capture}.renderer-owner").exists() || StillOutputStore.claimForRenderer(context.filesDir, job.capture)) { "Capture is still processing" }
            repository.captureEntry(job.capture)?.let { entry ->
                entry.targets.filterNot { it.published }.forEach { target -> runCatching {
                    val uri = target.uri.toUri()
                    if (uri.authority != "media" || !output.isPublished(uri)) output.delete(uri)
                } }
                repository.removeCaptureEntry(entry)
            }
            StillOutputStore.rendererRelease(context.filesDir, job.capture)
        }
        if (job.status != "Complete" && job.output.isNotEmpty()) runCatching {
            val uri = job.output.toUri()
            if (!output.isPublished(uri)) output.delete(uri)
        }
        repository.cancelDraftWrite(job.id)
        assets.release(job)
        repository.deleteJobFiles(job.id)
    }
    companion object {
        private const val IMPORT_CHECK_BYTES = 64L * 1024 * 1024
        // get() constructs this process-lifetime store exclusively with applicationContext.
        @android.annotation.SuppressLint("StaticFieldLeak")
        @Volatile private var instance: RendererStore? = null
        fun get(context: Context): RendererStore = instance ?: synchronized(this) {
            instance ?: RendererStore(context.applicationContext).also { instance = it }
        }
    }
}
