package com.rawr.camera.renderer

import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeoutOrNull
import org.json.JSONObject

internal data class RendererPreviewKey(val draft: String, val megapixels: Double)
internal fun RenderJob.previewKey(): RendererPreviewKey {
    val pixels = runCatching {
        JSONObject(draft).apply {
            remove("export")
            remove("editorProfiles")
            remove("editorTones")
            remove("editorAgx")
            remove("editorFilmPresets")
            remove("editorFilmPresetId")
            remove("ultraHdrEnabled")
            optJSONObject("tone")?.apply { remove("jpegQuality"); remove("jpegChromaSubsamplingId") }
        }.toString()
    }.getOrDefault(draft)
    return RendererPreviewKey(pixels, megapixels)
}

internal data class RendererPreviewRequest(
    val job: RenderJob,
    val viewport: RendererViewport = RendererViewport(),
    val visible: Boolean = true,
    val exporting: Boolean = false,
    val waveformGeneration: Long = 0,
    val waveform: Boolean = false,
    val navigationEdge: Int = navigationEdge(viewport)
) {
    val overviewKey get() = job.previewKey() to waveformGeneration
    // Fitted refinement is the same image regardless of the panel/canvas size.
    val refinementKey get() = Triple(overviewKey, navigationEdge, viewport.takeIf { it.zoom > 1.05f })
    val eligible get() = visible && !exporting && job.width > 0 && viewport.width > 1 && viewport.height > 1
}

/** One owner for both preview qualities. Latest edits get an overview first;
 * refinement starts only after requests settle. Native work never overlaps.
 * Viewport changes don't abort RAW preparation or rebuild the fitted image.
 */
internal class RendererProxyScheduler(
    scope: CoroutineScope,
    private val requests: StateFlow<RendererPreviewRequest>,
    overview: suspend (RendererPreviewRequest) -> Unit,
    refine: suspend (RendererPreviewRequest) -> Unit,
    onFailure: (Exception) -> Unit
) {
    private val wakeups = Channel<Unit>(Channel.CONFLATED)
    init {
        scope.launch { requests.collect { wakeups.trySend(Unit) } }
        scope.launch {
            var shown: Pair<RendererPreviewKey, Long>? = null
            var refined: Triple<Pair<RendererPreviewKey, Long>, Int, RendererViewport?>? = null
            suspend fun attempt(work: suspend () -> Unit) {
                try { work() }
                catch (e: CancellationException) { throw e }
                catch (e: Exception) { onFailure(e) }
            }
            for (ignored in wakeups) {
                while (true) {
                    val current = requests.value
                    if (!current.eligible) { shown = null; refined = null; break }
                    if (shown != current.overviewKey) {
                        attempt { overview(current) }
                        shown = current.overviewKey
                        continue
                    }
                    if (refined == current.refinementKey) break
                    val region = displayRegion(current.viewport)
                    if (region[2] <= 0f || region[3] <= 0f) break
                    // A new intent interrupts the quiet period, not an active GPU operation.
                    if (withTimeoutOrNull(350) { wakeups.receive(); true } == true) continue
                    if (requests.value != current) continue
                    attempt { refine(current) }
                    refined = current.refinementKey
                }
            }
        }
    }
}
