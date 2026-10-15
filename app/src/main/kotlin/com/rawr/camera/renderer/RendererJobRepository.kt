package com.rawr.camera.renderer

import android.content.Context
import com.rawr.camera.settings.model.AutoKeyExposure
import com.rawr.camera.settings.model.SettingsValues
import com.rawr.camera.storage.CaptureJournal
import com.rawr.camera.storage.StillOutputStore
import java.io.File
import java.io.FileOutputStream
import java.nio.file.Files
import java.nio.file.StandardCopyOption
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import org.json.JSONObject

/** Durable job records and optimistic draft persistence; no native rendering. */
internal class RendererJobRepository(private val context: Context, private val scope: CoroutineScope) {
    private val mutableJobs = MutableStateFlow<List<RenderJob>>(emptyList())
    val jobs = mutableJobs.asStateFlow()
    private val root = File(context.filesDir, "renderer_jobs").apply { mkdirs() }
    private val journal = CaptureJournal(File(context.filesDir, "still_jobs"))
    private val saveLane = Mutex()
    // Capture job ids carry the capture name (RAWR_<date>_<time>), hence "_".
    fun directory(id: String) = File(root, id).also { require(id.matches(Regex("[A-Za-z0-9._-]+")) && ".." !in id) }
    private fun read(file: File): RenderJob {
        val j = JSONObject(file.readText())
        return RenderJob(j.getString("id"), j.getString("name"), j.optString("capture"),
            j.optString("original"), j.optString("draft"), j.optInt("width"), j.optInt("height"),
            j.optInt("orientation", 1), j.optBoolean("rawr"), j.optDouble("megapixels", 0.0),
            j.optString("status", "Editing"), j.optString("error"), j.optString("output"), j.optString("runRecipe")).let { job ->
                if (RenderResolution.is200Mp(job.width, job.height) && job.megapixels == 0.0) job.copy(megapixels = 50.0) else job
            }
    }
    @Synchronized fun refresh() {
        val saved = root.listFiles().orEmpty().mapNotNull { d -> runCatching { read(File(d, "job.json")) }.getOrNull() }
        val queued = journal.readAll().filter { !it.complete && journal.hasInput(it) && saved.none { s -> s.capture == it.name } }
            .map { RenderJob("capture-${it.name}", it.name, capture = it.name, original = it.recipe,
                status = if (StillOutputStore.isClaimed(it.name)) "Processing capture" else "Unfinished capture", error = it.lastError) }
        mutableJobs.value = saved.map { disk -> if (draftWrites[disk.id]?.isActive == true) mutableJobs.value.firstOrNull { it.id == disk.id } ?: disk else disk } + queued
    }
    @Synchronized fun save(job: RenderJob) {
        val d = directory(job.id).apply { mkdirs() }
        val j = JSONObject().apply {
            put("id", job.id); put("name", job.name); put("capture", job.capture)
            put("original", job.original); put("draft", job.draft); put("width", job.width); put("height", job.height)
            put("orientation", job.orientation); put("rawr", job.rawr); put("megapixels", job.megapixels)
            put("runRecipe", job.runRecipe); put("status", job.status); put("error", job.error); put("output", job.output)
        }
        val temp = File(d, "job.tmp")
        FileOutputStream(temp).use { it.write(j.toString().toByteArray()); it.fd.sync() }
        Files.move(temp.toPath(), File(d, "job.json").toPath(), StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE)
        refresh()
    }
    fun current(id: String) = mutableJobs.value.first { it.id == id }
    fun source(job: RenderJob) = File(directory(job.id), "input.dng")
    private val draftWrites = java.util.concurrent.ConcurrentHashMap<String, Job>()
    private val ready = CompletableDeferred<Unit>()
    init {
        scope.launch {
            try {
                refresh()
                ready.complete(Unit)
            } catch (error: Exception) {
                ready.completeExceptionally(error)
            }
        }
    }
    suspend fun awaitReady() = ready.await()
    fun updateDraft(id: String, values: SettingsValues) {
        draftWrites.remove(id)?.cancel()
        val job = current(id)
        writeDraft(job.copy(draft = RendererRecipe.rebuiltDraft(values, job.draft, job.original)))
    }
    fun updateAdaptiveToneMode(id: String, mode: AutoKeyExposure) {
        draftWrites.remove(id)?.cancel()
        val job = current(id)
        writeDraft(job.copy(draft = RendererRecipe.withAdaptiveToneMode(job.draft, mode)))
    }
    private fun writeDraft(job: RenderJob) {
        val updated = job.copy(status = "Editing", error = "")
        mutableJobs.value = mutableJobs.value.map { if (it.id == job.id) updated else it }
        draftWrites[job.id] = scope.launch { delay(500); saveLane.withLock { save(current(job.id)) } }
    }
    fun cancelDraftWrite(id: String) { draftWrites.remove(id)?.cancel() }
    fun checkImportSpace(bytes: Long) {
        require(bytes >= 0) { "Invalid DNG size" }
        val storage = context.getSystemService(android.os.storage.StorageManager::class.java)
        val volume = storage.getUuidForPath(root)
        val required = bytes + 64L * 1024 * 1024
        check(storage.getAllocatableBytes(volume) >= required) { "Not enough storage to import DNG" }
        storage.allocateBytes(volume, required)
    }
    fun captureEntry(name: String) = journal.readAll().firstOrNull { it.name == name }
    fun removeCaptureEntry(entry: CaptureJournal.Entry) = journal.remove(entry)
    fun deleteJobFiles(id: String) {
        cancelDraftWrite(id)
        directory(id).deleteRecursively()
        refresh()
    }
}
