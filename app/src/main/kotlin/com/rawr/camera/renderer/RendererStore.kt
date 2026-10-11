package com.rawr.camera.renderer

import androidx.core.net.toUri
import android.content.Context
import android.net.Uri
import android.os.ParcelFileDescriptor
import android.view.Surface
import com.rawr.camera.settings.model.SettingsValues
import com.rawr.camera.storage.StillOutputStore
import java.io.File
import java.io.FileOutputStream
import java.nio.file.Files
import java.nio.file.StandardCopyOption
import java.util.UUID
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
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
    suspend fun preview(job: RenderJob, region: FloatArray? = null) = previews.preview(job, region)
    suspend fun attachPreviewSurface(job: RenderJob, surface: Surface?) = previews.attachPreviewSurface(job, surface)
    fun detachPreviewSurface(after: Job? = null) = previews.detachPreviewSurface(after)
    suspend fun previewSurface(job: RenderJob) = previews.previewSurface(job)
    suspend fun previewHd(job: RenderJob) = previews.previewHd(job)
    suspend fun closePreview() = previews.closePreview()
    fun releasePreview() = previews.releasePreview()
    fun cancel() = exports.cancel()
    fun hasInterruptedExport() = exports.hasInterruptedExport()
    fun resumeInterrupted() = exports.resumeInterrupted()
    fun export(id: String, original: Boolean = false, resume: Boolean = false) = exports.export(id, original, resume)
    suspend fun import(uri: Uri, library: SettingsValues): RenderJob = withContext(Dispatchers.IO) {
        lane.withLock {
            val id = UUID.randomUUID().toString(); val d = repository.directory(id).apply { mkdirs() }
            try {
                val name = context.contentResolver.query(uri, arrayOf(android.provider.OpenableColumns.DISPLAY_NAME), null, null, null)?.use {
                    if (it.moveToFirst()) it.getString(0) else "Imported DNG"
                } ?: "Imported DNG"
                val temp = File(d, "input.tmp")
                context.contentResolver.openInputStream(uri)!!.use { input -> FileOutputStream(temp).use { out ->
                    val buffer = ByteArray(1024 * 1024)
                    while (true) { val n = input.read(buffer); if (n < 0) break
                        repository.checkImportSpace(n); out.write(buffer, 0, n)
                    }; out.fd.sync()
                } }
                Files.move(temp.toPath(), File(d, "input.dng").toPath(), StandardCopyOption.ATOMIC_MOVE)
                inspect(RenderJob(id, name), library).also(::save)
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
        val original = job.original.ifBlank { ext?.optString("com.rawrcam.processing.recipe.v1").orEmpty() }
            .ifBlank { RendererRecipe.encode(RendererRecipe.neutral(library)) }
        val resolved = ext?.optString("com.rawrcam.processing.resolved.v1")?.takeIf { it.isNotBlank() }?.let(::JSONObject)
        val frame = ext?.optString("com.rawrcam.processing.frame.v1")?.takeIf { it.isNotBlank() }?.let(::JSONObject)
        val frozen = JSONObject(original).apply {
            put("aePostGain", resolved?.optDouble("aePostGain", 1.0) ?: 1.0)
            resolved?.optJSONObject("tonemap")?.optJSONObject("adaptiveTone")?.let { put("adaptiveTone", it) }
            put("timestampNs", frame?.optLong("timestampNs", 1_000_000_000) ?: 1_000_000_000)
            if (frame != null) put("resolvedFrame", frame)
        }.toString().let(assets::resolve)
        RendererRecipe.decode(frozen, library) // Validate before the editor is composed.
        return job.copy(original = frozen, draft = frozen, width = w, height = h, rawr = rawr,
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
                inspect(job, library).also(::save)
            } catch (error: Exception) {
                runCatching { save(job.copy(status = "Failed", error = error.message.orEmpty())) }
                if (!File(repository.directory(job.id), "job.json").exists()) StillOutputStore.rendererRelease(context.filesDir, job.capture)
                throw error
            }
        }
    }
    suspend fun remove(job: RenderJob) = withContext(Dispatchers.IO) { lane.withLock { check(!busy.value); removeUnlocked(job) } }
    private fun removeUnlocked(job: RenderJob) {
        native.closeFor(job.id)
        if (job.capture.isNotEmpty()) {
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
        // get() constructs this process-lifetime store exclusively with applicationContext.
        @android.annotation.SuppressLint("StaticFieldLeak")
        @Volatile private var instance: RendererStore? = null
        fun get(context: Context): RendererStore = instance ?: synchronized(this) {
            instance ?: RendererStore(context.applicationContext).also { instance = it }
        }
    }
}
