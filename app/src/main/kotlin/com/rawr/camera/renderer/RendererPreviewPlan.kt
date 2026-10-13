package com.rawr.camera.renderer

import org.json.JSONObject
import kotlin.math.ceil
import kotlin.math.floor
import kotlin.math.sqrt

internal data class RendererImageSize(val width: Int, val height: Int) {
    val pixels: Long get() = width.toLong() * height
}

/** Display-sized, aspect-preserving navigation image, never larger than the working source. */
internal fun fittedPreviewSize(job: RenderJob, edge: Int, maxPixels: Long = 2_000_000): RendererImageSize {
    val (w, h) = RenderResolution.dimensions(job.width, job.height, job.megapixels)
    val scale = minOf(1.0, edge.coerceAtLeast(2).toDouble() / maxOf(w, h), sqrt(maxPixels.coerceAtLeast(4).toDouble() / (w.toLong() * h)))
    return RendererImageSize((w * scale).toInt().coerceAtLeast(2), (h * scale).toInt().coerceAtLeast(2))
}
internal fun navigationEdge(view: RendererViewport): Int = ((maxOf(view.width, view.height) + 127) / 128) * 128

internal data class RendererPreviewBudget(val cacheBytes: Long, val maxRenderPixels: Long) {
    companion object {
        private const val MIB = 1024L * 1024
        fun admit(heapFree: Long, systemFree: Long, lowMemory: Boolean): RendererPreviewBudget {
            val heap = (heapFree - 32 * MIB).coerceAtLeast(0)
            val system = (systemFree - 128 * MIB).coerceAtLeast(0)
            // Include replacement bitmap/ARGB copies and a conservative native processing allowance.
            val pixels = minOf(2_000_000L, heap / 16, system / 256, if (lowMemory) 400_000L else Long.MAX_VALUE)
            val cache = minOf(64 * MIB, heap / 4, system / 16, if (lowMemory) 4 * MIB else Long.MAX_VALUE)
            return RendererPreviewBudget(cache, pixels)
        }
    }
}

/** Tiles are anchored to sensor coordinates, independent of pan position and EXIF rotation. */
internal data class RendererTileKey(val sample: Int, val x: Int, val y: Int, val width: Int, val height: Int) {
    val output get() = RendererImageSize((width + sample - 1) / sample, (height + sample - 1) / sample)
    val bytes get() = output.pixels * 4
    fun crop() = intArrayOf(x, y, width, height)
}
internal data class RendererTilePlan(val visible: List<RendererTileKey>, val prefetch: List<RendererTileKey>)

internal fun rendererTilePlan(job: RenderJob, view: RendererViewport, budget: RendererPreviewBudget): RendererTilePlan {
    if (view.zoom <= 1.05f || budget.cacheBytes < 1024 * 1024 || budget.maxRenderPixels < 768L * 768) return RendererTilePlan(emptyList(), emptyList())
    val region = detailRegion(job.orientation, view)
    if (region[2] <= 0 || region[3] <= 0) return RendererTilePlan(emptyList(), emptyList())
    val displayWidth = if (job.orientation in 5..8) job.height else job.width
    val pixelsPerSource = view.width * view.zoom / displayWidth
    val export = RenderResolution.dimensions(job.width, job.height, job.megapixels)
    var sample = if (RenderResolution.is200Mp(job.width, job.height)) 2 else 1
    while (sample < 32768 && (1f / (sample * 2) >= pixelsPerSource || job.width.toDouble() / sample > export.first * 1.001)) sample *= 2
    // Film, reconstruction and denoise can depend on region-wide statistics or broad kernels.
    // Keep them as one coherent cached patch rather than introducing seams from
    // independently toned tiles. The patch is still anchored/prefetched for reuse.
    val coherent = runCatching {
        JSONObject(job.draft).let { it.optBoolean("filmSimEnabled", false) || it.optBoolean("highlightReconstructionEnabled", true) || it.optBoolean("denoiseEnabled", false) }
    }.getOrDefault(true)
    if (coherent) {
        fun patch(): RendererTileKey {
            val grid = 128 * sample
            val x = (floor(region[0] * job.width / grid).toInt() * grid).coerceAtLeast(0)
            val y = (floor(region[1] * job.height / grid).toInt() * grid).coerceAtLeast(0)
            val right = (ceil((region[0] + region[2]) * job.width / grid).toInt() * grid).coerceAtMost(job.width)
            val bottom = (ceil((region[1] + region[3]) * job.height / grid).toInt() * grid).coerceAtMost(job.height)
            return RendererTileKey(sample, x, y, right - x, bottom - y)
        }
        var key = patch()
        while (((key.output.width + 256L) * (key.output.height + 256L) > budget.maxRenderPixels ||
                    key.bytes > budget.cacheBytes / 2) && sample < 32768) {
            sample *= 2; key = patch()
        }
        return if (key.width > 0 && key.height > 0 && key.bytes <= budget.cacheBytes / 2)
            RendererTilePlan(listOf(key), emptyList()) else RendererTilePlan(emptyList(), emptyList())
    }
    fun keys(sampleSize: Int, border: Int): List<RendererTileKey> {
        val span = 512 * sampleSize
        val left = (floor(region[0] * job.width / span).toInt() - border).coerceAtLeast(0)
        val top = (floor(region[1] * job.height / span).toInt() - border).coerceAtLeast(0)
        val right = (ceil((region[0] + region[2]) * job.width / span).toInt() + border).coerceAtMost((job.width + span - 1) / span)
        val bottom = (ceil((region[1] + region[3]) * job.height / span).toInt() + border).coerceAtMost((job.height + span - 1) / span)
        val cx = (region[0] + region[2] / 2) * job.width
        val cy = (region[1] + region[3] / 2) * job.height
        return buildList {
            for (row in top until bottom) for (column in left until right) {
                val x = column * span; val y = row * span
                add(RendererTileKey(sampleSize, x, y, minOf(span, job.width - x), minOf(span, job.height - y)))
            }
        }.sortedBy { kotlin.math.abs(it.x + it.width / 2 - cx) + kotlin.math.abs(it.y + it.height / 2 - cy) }
    }
    var visible = keys(sample, 0)
    // Leave room for old displayed tiles and incoming replacements; never allocate a screen-sized burst unbounded.
    while ((visible.size > 24 || visible.sumOf { it.bytes } > budget.cacheBytes / 2) && sample < 32768) {
        sample *= 2
        visible = keys(sample, 0)
    }
    if (visible.sumOf { it.bytes } > budget.cacheBytes) return RendererTilePlan(emptyList(), emptyList())
    val prefetch = keys(sample, 1).filterNot { it in visible }.take(4)
    return RendererTilePlan(visible, prefetch)
}

/** Native returns each tile already EXIF-oriented; place it in the corresponding display rectangle. */
internal fun tileDisplayRegion(tile: RendererTileKey, job: RenderJob): FloatArray {
    val x = tile.x.toFloat() / job.width; val y = tile.y.toFloat() / job.height
    val w = tile.width.toFloat() / job.width; val h = tile.height.toFloat() / job.height
    return when (job.orientation) {
        2 -> floatArrayOf(1-x-w, y, w, h)
        3 -> floatArrayOf(1-x-w, 1-y-h, w, h)
        4 -> floatArrayOf(x, 1-y-h, w, h)
        5 -> floatArrayOf(y, x, h, w)
        6 -> floatArrayOf(1-y-h, x, h, w)
        7 -> floatArrayOf(1-y-h, 1-x-w, h, w)
        8 -> floatArrayOf(y, 1-x-w, h, w)
        else -> floatArrayOf(x, y, w, h)
    }
}

/** Published images are immutable and never recycled while Compose may retain them. */
internal class RendererTileCache<T>(var limitBytes: Long) {
    private val entries = LinkedHashMap<RendererTileKey, T>(16, .75f, true)
    var bytes = 0L
        private set
    operator fun get(key: RendererTileKey): T? = entries[key]
    fun put(key: RendererTileKey, value: T, protected: Set<RendererTileKey>): Boolean {
        if (key.bytes > limitBytes) return false
        if (key in entries) { entries[key] = value; return true }
        while (bytes + key.bytes > limitBytes) {
            val victim = entries.keys.firstOrNull { it !in protected } ?: return false
            entries.remove(victim); bytes -= victim.bytes
        }
        entries[key] = value; bytes += key.bytes
        return true
    }
    fun complete(keys: List<RendererTileKey>): Map<RendererTileKey, T>? {
        if (keys.any { it !in entries }) return null
        return keys.associateWith { entries.getValue(it) }
    }
    fun clear() { entries.clear(); bytes = 0 }
}
