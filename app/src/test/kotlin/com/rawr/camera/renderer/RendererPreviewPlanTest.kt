package com.rawr.camera.renderer

import kotlin.test.*

class RendererPreviewPlanTest {
    private val job = RenderJob("test", "200MP", draft = """{"highlightReconstructionEnabled":false}""", width = 16304, height = 12240, megapixels = 50.0)
    private val budget = RendererPreviewBudget(64L * 1024 * 1024, 2_000_000)

    @Test fun fittedPreviewTracksScreenAndNeverAllocatesSixMp() {
        val small = fittedPreviewSize(job, 768)
        val large = fittedPreviewSize(job, 4096)
        assertTrue(small.width <= 768 && small.pixels < 600_000)
        assertTrue(large.pixels <= 2_000_000)
        assertTrue(kotlin.math.abs(small.width.toDouble() / small.height - job.width.toDouble() / job.height) < .01)
        assertEquals(RendererImageSize(100, 80), fittedPreviewSize(job.copy(width = 100, height = 80, megapixels = 0.0), 2048))
    }
    @Test fun panReusesSourceAnchoredTilesAndPrefetchIsBounded() {
        val view = RendererViewport(zoom = 8f, width = 800, height = 600)
        val a = rendererTilePlan(job, view, budget)
        val b = rendererTilePlan(job, view.copy(offsetX = 50f), budget)
        assertTrue(a.visible.isNotEmpty())
        assertTrue(a.visible.intersect(b.visible.toSet()).isNotEmpty())
        assertTrue(a.prefetch.size <= 4)
        assertTrue(a.visible.all { it.sample >= 2 && it.output.width <= 512 && it.output.height <= 512 })
        assertTrue(a.visible.sumOf { it.bytes } <= budget.cacheBytes / 2)
    }
    @Test fun spatialLooksUseOneCoherentCachedRegion() {
        val view = RendererViewport(zoom = 8f, width = 800, height = 600)
        for (draft in listOf("""{"filmSimEnabled":true}""", """{"highlightReconstructionEnabled":true}""", """{"denoiseEnabled":true,"highlightReconstructionEnabled":false}""")) {
            val plan = rendererTilePlan(job.copy(draft = draft), view, budget)
            assertEquals(1, plan.visible.size)
            assertTrue(plan.prefetch.isEmpty())
            val size = plan.visible.single().output
            assertTrue((size.width + 256L) * (size.height + 256L) <= budget.maxRenderPixels)
        }
    }
    @Test fun noDetailIsQueuedAtFitOrOutsideImage() {
        assertTrue(rendererTilePlan(job, RendererViewport(width = 800, height = 600), budget).visible.isEmpty())
        assertTrue(rendererTilePlan(job, RendererViewport(zoom = 8f, offsetX = 100000f, width = 800, height = 600), budget).visible.isEmpty())
    }
    @Test fun memoryAdmissionShrinksWorkAndRefusesDetailWhenInsufficient() {
        val normal = RendererPreviewBudget.admit(512L shl 20, 4L shl 30, false)
        val low = RendererPreviewBudget.admit(64L shl 20, 256L shl 20, true)
        val exhausted = RendererPreviewBudget.admit(16L shl 20, 64L shl 20, true)
        assertTrue(low.cacheBytes < normal.cacheBytes)
        assertTrue(low.maxRenderPixels < normal.maxRenderPixels)
        assertEquals(0, exhausted.maxRenderPixels)
        assertTrue(rendererTilePlan(job, RendererViewport(zoom = 8f, width = 800, height = 600), exhausted).visible.isEmpty())
    }
    @Test fun allExifOrientationsMapTilesToTheSameSourceRegion() {
        val tile = RendererTileKey(2, 2048, 1024, 4096, 2048)
        for (orientation in 1..8) {
            val region = tileDisplayRegion(tile, job.copy(orientation = orientation))
            // Build a viewport whose visible normalized rectangle is exactly this tile.
            val width = 10000; val height = 10000
            val canvasWidth = (region[2] * width).toInt()
            val canvasHeight = (region[3] * height).toInt()
            val view = RendererViewport(zoom = 2f,
                offsetX = -region[0] * width - (canvasWidth - width) / 2f,
                offsetY = -region[1] * height - (canvasHeight - height) / 2f,
                width = width / 2, height = height / 2, canvasWidth = canvasWidth, canvasHeight = canvasHeight)
            val sensor = detailRegion(orientation, view)
            val expected = floatArrayOf(tile.x.toFloat() / job.width, tile.y.toFloat() / job.height,
                tile.width.toFloat() / job.width, tile.height.toFloat() / job.height)
            for (i in 0..3) assertTrue(kotlin.math.abs(expected[i] - sensor[i]) < .0002f, "EXIF $orientation component $i")
        }
    }
    @Test fun lruBudgetProtectsVisibleTilesAndDoesNotPublishPartialLayers() {
        val a = RendererTileKey(2, 0, 0, 1024, 1024)
        val b = a.copy(x = 1024)
        val c = a.copy(x = 2048)
        val cache = RendererTileCache<String>(a.bytes * 2)
        assertTrue(cache.put(a, "a", emptySet()))
        assertNull(cache.complete(listOf(a, b)))
        assertTrue(cache.put(b, "b", setOf(a)))
        assertEquals(mapOf(a to "a", b to "b"), cache.complete(listOf(a, b)))
        assertTrue(cache.put(c, "c", setOf(a)))
        assertNull(cache[b]); assertEquals("a", cache[a]); assertEquals("c", cache[c])
        assertTrue(cache.bytes <= cache.limitBytes)
        cache.clear(); assertEquals(0, cache.bytes); assertNull(cache.complete(listOf(a)))
    }
}
