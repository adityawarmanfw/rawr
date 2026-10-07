package com.rawr.camera.renderer

import android.content.Context
import androidx.core.net.toUri
import android.os.Build
import android.provider.MediaStore
import com.rawr.camera.settings.model.SettingsCatalog
import com.rawr.camera.settings.model.describeFilm
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.TimeZone
import java.util.concurrent.atomic.AtomicLong
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import org.json.JSONObject

/** Export admission, durable transitions, progress, and interrupted-publication recovery. */
internal class RendererExportCoordinator(
    private val context: Context,
    private val scope: CoroutineScope,
    private val lane: Mutex,
    private val repository: RendererJobRepository,
    private val native: RendererNativeSession,
    private val assets: RendererAssets,
    private val output: RendererOutputPublisher,
    private val visible: () -> Boolean,
    private val removeCompleted: (RenderJob) -> Unit
) {
    private val mutableBusy = MutableStateFlow(false)
    val busy = mutableBusy.asStateFlow()
    private val mutableCompletion = MutableStateFlow<String?>(null)
    val completion = mutableCompletion.asStateFlow()
    private val mutableProgress = MutableStateFlow(0)
    val progress = mutableProgress.asStateFlow()
    @Volatile private var cancelled = false
    private val lastExportTimestamp = AtomicLong(0)
    fun cancel() { cancelled = true; native.cancel() }
    fun hasInterruptedExport() = repository.jobs.value.any { it.status in setOf("Rendering", "Publishing", "Complete") }
    fun resumeInterrupted() {
        if (!mutableBusy.value) repository.jobs.value.firstOrNull { it.status in setOf("Rendering", "Publishing", "Complete") }?.let { export(it.id, resume = true) }
    }
    fun export(id: String, original: Boolean = false, resume: Boolean = false) {
        check(!mutableBusy.value)
        val admitted = repository.current(id)
        repository.cancelDraftWrite(id)
        mutableBusy.value = true; cancelled = false
        try { RendererService.start(context) } catch (e: Exception) { mutableBusy.value = false; throw e }
        scope.launch {
            val ticker = launch { while (isActive) { mutableProgress.value = native.progress(); delay(250) } }
            try { lane.withLock {
                var job = admitted; check(job.width > 0) { "Open this capture before rendering" }
                if (job.output.isNotEmpty() && (job.status == "Complete" || output.isPublished(job.output.toUri()))) {
                    removeCompleted(job.copy(status = "Complete")); return@withLock
                }
                if (resume && job.status == "Publishing" && job.output.isNotEmpty()) {
                    val uri = job.output.toUri()
                    output.publish(uri)
                    job = job.copy(status = "Complete"); repository.save(job)
                    runCatching { RendererService.completed(context, visible(), uri) }
                    removeCompleted(job); return@withLock
                }
                check(!cancelled) { "Cancelled" }
                val requestedRecipe = if (resume) job.runRecipe.ifBlank { job.draft } else if (original) job.original else job.draft
                val recipe = assets.prepareRecipe(job, requestedRecipe)
                assets.validate(recipe)
                job = job.copy(status = "Rendering", error = "", runRecipe = recipe); repository.save(job)
                // Still-style export identity: RAWR_{timestamp}_R{resolution}.jpg
                // (R50/R25/R12 for downscaled exports, bare R at native
                // resolution). Stamped into MediaStore and the JPEG EXIF alike.
                val exportMillis = lastExportTimestamp.updateAndGet { prev -> maxOf(System.currentTimeMillis(), prev + 1) }
                val resSuffix = if (job.megapixels == 0.0) "R" else "R" + job.megapixels.toInt()
                val exportBase = SimpleDateFormat("yyyyMMdd_HHmmss_SSS", Locale.US).format(Date(exportMillis))
                val displayName = "RAWR_${exportBase}_$resSuffix.jpg"
                val decoded = RendererRecipe.decode(recipe, SettingsCatalog.initialState().values)
                val exportRecipe = JSONObject(recipe).apply {
                    put("export", JSONObject()
                        .put("displayName", displayName)
                        .put("wallClockMillis", exportMillis)
                        .put("utcOffsetMinutes", TimeZone.getDefault().getOffset(exportMillis) / 60_000)
                        .put("deviceMake", Build.MANUFACTURER)
                        .put("deviceModel", com.rawr.camera.integration.DeviceNames.marketingModel())
                        .put("filmDescription", if (decoded.filmSimEnabled) decoded.filmSimLook.describeFilm() else "")
                        .put("rendererDisplayName", decoded.captureRendererDisplayName()))
                }.toString()
                job = job.copy(runRecipe = exportRecipe); repository.save(job)
                val (w, h) = RenderResolution.dimensions(job.width, job.height, job.megapixels)
                val uri = if (job.output.isNotEmpty()) job.output.toUri() else output.createPending(displayName)
                job = job.copy(output = uri.toString()); repository.save(job)
                output.write(uri) { fd -> native.render(job, job.runRecipe, w, h, fd) }
                check(!cancelled) { "Cancelled" }
                repository.save(job.copy(status = "Publishing"))
                output.publish(uri)
                job = job.copy(status = "Complete"); repository.save(job)
                mutableCompletion.value = uri.toString()
                runCatching { RendererService.completed(context, visible(), uri) }
                removeCompleted(job)
            } } catch (error: Exception) {
                repository.jobs.value.firstOrNull { it.id == id }?.let { runCatching { repository.save(it.copy(status = if (it.status == "Complete") "Complete" else "Failed", error = error.message ?: "Render failed")) } }
            } finally { ticker.cancel(); mutableBusy.value = false; RendererService.stop(context) }
        }
    }
}
