package com.rawr.camera.renderer

import android.graphics.Bitmap
import org.json.JSONObject
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext

/** One immutable result: scope pixels never publish independently of their image. */
internal data class RendererPreviewPixels(val bitmap: Bitmap, val waveform: Bitmap?)

/** Android output adapter; native work and readback share the operation lane. */
internal class RendererPreviewBackend(
    private val native: RendererNativeSession,
    private val assets: RendererAssets,
    private val lane: Mutex,
    private val scope: CoroutineScope,
    private val exporting: () -> Boolean,
    private val visible: () -> Boolean
) {
    suspend fun render(job: RenderJob, size: RendererImageSize, refined: Boolean, waveform: Boolean,
        crop: IntArray? = null): RendererPreviewPixels = withContext(Dispatchers.IO) {
        lane.withLock {
            check(!exporting()) { "Export in progress" }
            val export = RenderResolution.dimensions(job.width, job.height, job.megapixels)
            val recipe = assets.prepareRecipe(job, job.draft).let {
                if (waveform) JSONObject(it).put("rendererWaveform", true).toString() else it
            }
            val region = crop ?: if (refined) intArrayOf(0, 0, job.width, job.height) else null
            val pixels = checkNotNull(native.render(job, recipe, size.width, size.height, -1, region, export.first, export.second))
            val (bw, bh) = if (job.orientation in 5..8) size.height to size.width else size.width to size.height
            val bitmap = Bitmap.createBitmap(pixels, bw, bh, Bitmap.Config.ARGB_8888)
            val scope = if (waveform) native.waveform(job)?.let {
                Bitmap.createBitmap(it, 512, 384, Bitmap.Config.ARGB_8888)
            } else null
            RendererPreviewPixels(bitmap, scope)
        }
    }
    suspend fun closePreview() {
        check(!exporting())
        native.cancel()
        lane.withLock { native.close() }
    }
    fun releasePreview() { scope.launch { lane.withLock {
        if (!visible() && !exporting()) native.close()
    } } }
}
