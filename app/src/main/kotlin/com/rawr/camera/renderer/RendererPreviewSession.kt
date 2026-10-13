package com.rawr.camera.renderer

import android.graphics.Bitmap
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.*
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

internal data class RendererViewport(
    val zoom: Float = 1f,
    val offsetX: Float = 0f,
    val offsetY: Float = 0f,
    val width: Int = 1,
    val height: Int = 1,
    val canvasWidth: Int = width,
    val canvasHeight: Int = height
)

internal data class RendererPreviewFrame(
    val revision: Pair<RendererPreviewKey, Long>,
    val bitmap: Bitmap,
    val waveform: Bitmap?,
    val tiles: Map<RendererTileKey, Bitmap> = emptyMap()
)
internal data class RendererPreviewState(
    val frame: RendererPreviewFrame? = null,
    val failed: Boolean = false,
    val activity: String = ""
)

/** One publication owner for navigation, detail and scopes. UI changes never block on native work. */
internal class RendererPreviewSession(
    initialJob: RenderJob,
    private val store: RendererStore,
    private val onError: (String?) -> Unit
) : AutoCloseable {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private val requests = MutableStateFlow(RendererPreviewRequest(initialJob))
    private val mutableState = MutableStateFlow(RendererPreviewState())
    val state = mutableState.asStateFlow()
    private var budget = store.previewBudget()
    private val tiles = RendererTileCache<Bitmap>(budget.cacheBytes)
    private var fitted: Pair<Pair<RendererPreviewKey, Long>, RendererImageSize>? = null
    private val memory = store.observePreviewMemory { scope.launch {
        budget = store.previewBudget()
        tiles.clear(); tiles.limitBytes = budget.cacheBytes
        fitted = null
        mutableState.update { it.copy(frame = it.frame?.copy(tiles = emptyMap())) }
    } }

    init {
        RendererProxyScheduler(scope, requests,
            overview = { request ->
                render(request, "UPDATING PREVIEW") {
                    budget = store.previewBudget()
                    if (budget.cacheBytes < tiles.limitBytes) tiles.clear()
                    tiles.limitBytes = budget.cacheBytes
                    check(budget.maxRenderPixels >= 64 * 64) { "Not enough memory for preview" }
                    val size = fittedPreviewSize(request.job, minOf(1280, request.navigationEdge), budget.maxRenderPixels)
                    val result = store.previewPixels(request.job, size, refined = false, request.waveform)
                    if (current(request)) {
                        val old = mutableState.value.frame
                        val retained = if (old?.revision?.first == request.overviewKey.first) old.tiles else emptyMap()
                        mutableState.update { it.copy(frame = RendererPreviewFrame(request.overviewKey, result.bitmap, result.waveform, retained), failed = false) }
                    }
                }
            },
            refine = { request ->
                render(request, "PREPARING DETAIL") {
                    check(budget.maxRenderPixels >= 64 * 64) { "Not enough memory for preview" }
                    val size = fittedPreviewSize(request.job, request.navigationEdge, budget.maxRenderPixels)
                    if (fitted != (request.overviewKey to size) || mutableState.value.frame?.revision != request.overviewKey) {
                        val result = store.previewPixels(request.job, size, refined = true, request.waveform)
                        if (!current(request)) return@render
                        fitted = request.overviewKey to size
                        mutableState.update { it.copy(frame = RendererPreviewFrame(request.overviewKey, result.bitmap, result.waveform), failed = false) }
                    }
                    if (requests.value.refinementKey != request.refinementKey) return@render
                    val plan = rendererTilePlan(request.job, request.viewport, budget)
                    publishTiles(request, plan)
                    for (tile in plan.visible + plan.prefetch) {
                        if (!current(request) || requests.value.refinementKey != request.refinementKey) break
                        if (tiles[tile] == null) {
                            // Read back only the tile core; native supplies neighboring pixels for filters.
                            val result = store.previewPixels(request.job, tile.output, refined = true, waveform = false, crop = tile.crop())
                            if (!current(request)) break
                            tiles.put(tile, result.bitmap, plan.visible.toSet())
                        }
                        publishTiles(request, plan)
                    }
                }
            },
            onFailure = { error ->
                if (error !is IllegalStateException || error.message !in setOf("Cancelled", "Export in progress")) {
                    mutableState.update { it.copy(failed = true) }
                    onError(error.message)
                }
            })
        scope.launch { store.busy.collect { busy -> requests.update { it.copy(exporting = busy) } } }
    }

    private fun current(request: RendererPreviewRequest) =
        requests.value.overviewKey == request.overviewKey && requests.value.eligible

    private fun publishTiles(request: RendererPreviewRequest, plan: RendererTilePlan) {
        if (!current(request) || requests.value.refinementKey != request.refinementKey || plan.visible.isEmpty()) return
        val ready = tiles.complete(plan.visible) ?: return
        mutableState.update { state ->
            val frame = state.frame
            if (frame?.revision == request.overviewKey) state.copy(frame = frame.copy(tiles = ready)) else state
        }
    }

    private suspend fun render(request: RendererPreviewRequest, initial: String, work: suspend () -> Unit) = coroutineScope {
        val started = android.os.SystemClock.elapsedRealtime()
        mutableState.update { it.copy(activity = initial) }
        val progress = launch {
            while (true) {
                delay(250)
                val stage = withContext(Dispatchers.IO) { store.previewStage() }
                val label = when (stage) {
                    1 -> "BUILDING PREVIEW"
                    2 -> "BINNING 200 → 50 MP"
                    3 -> "DEMOSAICING"
                    4 -> "RESIZING PREVIEW"
                    5 -> "APPLYING LOOK"
                    else -> initial
                }
                mutableState.update { it.copy(activity = label) }
            }
        }
        try { work() }
        catch (error: OutOfMemoryError) {
            tiles.clear(); fitted = null
            mutableState.update { it.copy(frame = it.frame?.copy(tiles = emptyMap())) }
            throw IllegalStateException("Preview memory exhausted; retained the last image", error)
        }
        finally {
            progress.cancel()
            mutableState.update { it.copy(activity = "") }
            android.util.Log.i("RawrRenderer", "$initial ${request.job.width}x${request.job.height}: ${android.os.SystemClock.elapsedRealtime()-started}ms")
        }
    }

    fun setWaveformEnabled(enabled: Boolean) {
        if (requests.value.waveform == enabled) return
        requests.update { it.copy(waveform = enabled, waveformGeneration = it.waveformGeneration + 1) }
    }
    suspend fun metadata(): String = store.metadata(requests.value.job)
    fun updateJob(current: RenderJob) {
        if (requests.value.job.previewKey() != current.previewKey()) {
            store.cancelPreview()
            tiles.clear(); fitted = null
        }
        requests.update { it.copy(job = current) }
    }
    fun updateViewport(current: RendererViewport) {
        requests.update { it.copy(viewport = current, navigationEdge = maxOf(it.navigationEdge, navigationEdge(current))) }
        val request = requests.value
        publishTiles(request, rendererTilePlan(request.job, current, budget))
    }
    fun setVisible(value: Boolean) {
        requests.update { it.copy(visible = value) }
        if (!value) {
            store.cancelPreview(); tiles.clear(); fitted = null
            mutableState.update { it.copy(frame = it.frame?.copy(tiles = emptyMap())) }
        }
    }
    override fun close() {
        memory.close()
        store.cancelPreview()
        scope.cancel()
        tiles.clear()
    }
}

/** Intersection of the transformed image and canvas in display-image coordinates. */
internal fun displayRegion(view: RendererViewport): FloatArray {
    val w = (view.width * view.zoom).coerceAtLeast(1f)
    val h = (view.height * view.zoom).coerceAtLeast(1f)
    val x = (view.canvasWidth - w) / 2 + view.offsetX
    val y = (view.canvasHeight - h) / 2 + view.offsetY
    val left = (-x / w).coerceIn(0f, 1f)
    val top = (-y / h).coerceIn(0f, 1f)
    val right = ((view.canvasWidth - x) / w).coerceIn(0f, 1f)
    val bottom = ((view.canvasHeight - y) / h).coerceIn(0f, 1f)
    return floatArrayOf(left, top, (right-left).coerceAtLeast(0f), (bottom-top).coerceAtLeast(0f))
}

/** EXIF-aware mapping from the visible canvas intersection to sensor coordinates. */
internal fun detailRegion(orientation: Int, view: RendererViewport): FloatArray {
    val (left, top, width, height) = displayRegion(view)
    return when (orientation) {
        2 -> floatArrayOf(1-left-width, top, width, height)
        4 -> floatArrayOf(left, 1-top-height, width, height)
        5 -> floatArrayOf(top, left, height, width)
        7 -> floatArrayOf(1-top-height, 1-left-width, height, width)
        6 -> floatArrayOf(top, 1-left-width, height, width)
        8 -> floatArrayOf(1-top-height, left, height, width)
        3 -> floatArrayOf(1-left-width, 1-top-height, width, height)
        else -> floatArrayOf(left, top, width, height)
    }
}
