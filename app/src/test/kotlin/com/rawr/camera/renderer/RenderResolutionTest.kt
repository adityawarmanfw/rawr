package com.rawr.camera.renderer
import org.junit.Test
import org.junit.Assert.*
class RenderResolutionTest {
    @Test fun defaultsRespectSource() {
        assertEquals(50.0, RenderResolution.defaultMegapixels(16304, 12240, false), 0.0)
        assertEquals(0.0, RenderResolution.defaultMegapixels(9000, 6750, true), 0.0)
        assertEquals(0.0, RenderResolution.defaultMegapixels(4080, 3060, true), 0.0)
        // The cropped area of rawr-dng's 50MP output is slightly above 50M.
        assertEquals(0.0, RenderResolution.defaultMegapixels(8152, 6136, false), 0.0)
        assertTrue(RenderResolution.choices(8152, 6136, false).contains(0.0))
        assertEquals(25.0, RenderResolution.defaultMegapixels(9000, 6750, false), 0.0)
    }
    @Test fun highResolutionSourcesAlwaysHaveABinnedWorkingImage() {
        assertEquals(8152 to 6120, RenderResolution.dimensions(16304, 12240, 0.0))
        assertEquals(8152 to 6120, RenderResolution.dimensions(16304, 12240, 200.0))
        assertEquals(8152 to 6120, RenderResolution.workingDimensions(16304, 12240))
        assertFalse(RenderResolution.choices(16304, 12240, true).contains(0.0))
        assertEquals(4080 to 3060, RenderResolution.workingDimensions(4080, 3060))
        assertEquals(5760 to 4320, RenderResolution.workingDimensions(5760, 4320))
    }
    @Test fun dimensionsNeverUpscaleAndPreserveAspect() {
        assertEquals(4080 to 3060, RenderResolution.dimensions(4080, 3060, 0.0))
        for (mp in listOf(12.5, 25.0, 50.0)) {
            val (w, h) = RenderResolution.dimensions(16304, 12240, mp)
            assertEquals(mp, w.toLong() * h / 1e6, if (mp == 50.0) .15 else .03)
            assertEquals(16304.0 / 12240, w.toDouble() / h, .001)
        }
        assertEquals(101 to 99, RenderResolution.dimensions(101, 99, 25.0))
        assertFalse(RenderResolution.choices(16304, 12240, false).contains(0.0))
        assertTrue(RenderResolution.choices(9000, 6750, true).contains(0.0))
        assertEquals(8152 to 6120, RenderResolution.dimensions(16304, 12240, 50.0))
    }
    @Test fun fittedPreviewNeverExceedsSelectedOutputResolution() {
        val job = RenderJob("test", "test", width = 16304, height = 12240, megapixels = 12.5)
        val output = RenderResolution.dimensions(job.width, job.height, job.megapixels)
        val preview = fittedPreviewSize(job, 20000, maxPixels = 250_000_000)
        assertEquals(output.first, preview.width)
        assertEquals(output.second, preview.height)
    }
}
