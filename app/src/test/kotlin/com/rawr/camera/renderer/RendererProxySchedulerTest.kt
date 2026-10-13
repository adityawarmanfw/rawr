package com.rawr.camera.renderer

import kotlin.test.*
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.test.*

@OptIn(ExperimentalCoroutinesApi::class)
class RendererProxySchedulerTest {
    private fun request(draft: String = "A") = RendererPreviewRequest(
        RenderJob("job", "Job", draft = draft, width = 16000, height = 12000),
        RendererViewport(width = 400, height = 300))

    @Test fun newestOverviewPrecedesRefinementAndNativeCallsNeverOverlap() = runTest {
        val requests = MutableStateFlow(request())
        val release = CompletableDeferred<Unit>()
        val events = mutableListOf<String>()
        var active = 0
        var maxActive = 0
        RendererProxyScheduler(backgroundScope, requests, { r ->
            active++; maxActive = maxOf(maxActive, active)
            events += "overview ${r.job.draft}"
            if (r.job.draft == "A") release.await()
            active--
        }, { r ->
            active++; maxActive = maxOf(maxActive, active)
            events += "refine ${r.job.draft}"
            active--
        }, { throw it })
        runCurrent()
        requests.value = request("B")
        requests.value = request("C")
        runCurrent()
        assertEquals(listOf("overview A"), events)
        release.complete(Unit)
        runCurrent()
        assertEquals(listOf("overview A", "overview C"), events)
        advanceTimeBy(351); runCurrent()
        assertEquals(listOf("overview A", "overview C", "refine C"), events)
        assertEquals(1, maxActive)
    }

    @Test fun resizingFittedCanvasDoesNotRepeatOrCancelRefinement() = runTest {
        val requests = MutableStateFlow(request())
        val release = CompletableDeferred<Unit>()
        var overviews = 0
        var refinements = 0
        var completed = false
        RendererProxyScheduler(backgroundScope, requests, { overviews++ }, {
            refinements++
            release.await()
            completed = true
        }, { throw it })
        runCurrent(); advanceTimeBy(351); runCurrent()
        requests.value = requests.value.copy(viewport = RendererViewport(width = 200, height = 150))
        runCurrent()
        assertEquals(1, refinements)
        release.complete(Unit)
        runCurrent(); advanceTimeBy(1000); runCurrent()
        assertTrue(completed)
        assertEquals(1, overviews)
        assertEquals(1, refinements)
    }

    @Test fun panningRefinesNewestViewportWithoutRepeatingOverview() = runTest {
        val requests = MutableStateFlow(request())
        var overviews = 0
        val refined = mutableListOf<RendererViewport>()
        RendererProxyScheduler(backgroundScope, requests, { overviews++ }, { refined += it.viewport }, { throw it })
        runCurrent(); advanceTimeBy(351); runCurrent()
        requests.value = requests.value.copy(viewport = requests.value.viewport.copy(zoom = 2f))
        runCurrent(); advanceTimeBy(100)
        requests.value = requests.value.copy(viewport = requests.value.viewport.copy(offsetX = 20f))
        runCurrent(); advanceTimeBy(351); runCurrent()
        assertEquals(1, overviews)
        assertEquals(2, refined.size)
        assertEquals(20f, refined.last().offsetX)
    }

    @Test fun exportPausesBothQualitiesAndResumesNewestDraft() = runTest {
        val requests = MutableStateFlow(request().copy(exporting = true))
        val events = mutableListOf<String>()
        RendererProxyScheduler(backgroundScope, requests, { events += "overview ${it.job.draft}" },
            { events += "refine ${it.job.draft}" }, { throw it })
        runCurrent(); advanceTimeBy(1000); runCurrent()
        assertTrue(events.isEmpty())
        requests.value = request("B")
        runCurrent(); advanceTimeBy(351); runCurrent()
        assertEquals(listOf("overview B", "refine B"), events)
    }

    @Test fun hiddenCompletionDoesNotPreventRefreshOnReturn() = runTest {
        val requests = MutableStateFlow(request())
        val release = CompletableDeferred<Unit>()
        var refinements = 0
        RendererProxyScheduler(backgroundScope, requests, {}, {
            refinements++
            if (refinements == 1) release.await()
        }, { throw it })
        runCurrent(); advanceTimeBy(351); runCurrent()
        requests.value = requests.value.copy(visible = false)
        runCurrent(); release.complete(Unit); runCurrent()
        requests.value = requests.value.copy(visible = true)
        runCurrent(); advanceTimeBy(351); runCurrent()
        assertEquals(2, refinements)
    }

    @Test fun waveformChangeRefreshesBothQualities() = runTest {
        val requests = MutableStateFlow(request())
        var overviews = 0; var refinements = 0
        RendererProxyScheduler(backgroundScope, requests, { overviews++ }, { refinements++ }, { throw it })
        runCurrent(); advanceTimeBy(351); runCurrent()
        requests.value = requests.value.copy(waveformGeneration = 1)
        runCurrent(); advanceTimeBy(351); runCurrent()
        assertEquals(2, overviews); assertEquals(2, refinements)
    }

    @Test fun failedRequestDoesNotSpinRetrying() = runTest {
        val requests = MutableStateFlow(request())
        var errors = 0
        RendererProxyScheduler(backgroundScope, requests, { error("overview") }, { error("refine") }, { errors++ })
        runCurrent(); advanceTimeBy(2000); runCurrent()
        assertEquals(2, errors)
        requests.value = request("B")
        runCurrent(); advanceTimeBy(351); runCurrent()
        assertEquals(4, errors)
    }

    @Test fun outputDimensionsInvalidatePreviewButJpegQualityDoesNot() {
        val a = request("""{"tone":{"contrast":12,"jpegQuality":98,"jpegChromaSubsamplingId":"jpeg.420"}}""")
        val b = request("""{"tone":{"contrast":12,"jpegQuality":100,"jpegChromaSubsamplingId":"jpeg.444"}}""")
        assertEquals(a.overviewKey, b.overviewKey)
        assertNotEquals(a.overviewKey, a.copy(job = a.job.copy(megapixels = 12.5)).overviewKey)
    }
}
