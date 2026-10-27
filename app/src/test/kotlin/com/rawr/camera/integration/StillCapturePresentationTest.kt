package com.rawr.camera.integration

import com.rawr.camera.integration.StillCaptureCoordinator.CompletionEvent
import com.rawr.camera.integration.StillCaptureCoordinator.CompletionEvent.Artifact
import com.rawr.camera.model.*
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.test.*
import kotlin.test.*

@OptIn(ExperimentalCoroutinesApi::class)
class StillCapturePresentationTest {
    private class Fixture(scope: TestScope) {
        val state = MutableStateFlow(CaptureUiState(NativeCapabilityProjection.pending()))
        val presenter = StillCapturePresentation(scope.backgroundScope) { state.update(it) }
        fun capture(id: Long) = state.value.pendingCaptures.single { it.id == id }
        fun event(id: Long, artifact: Artifact, success: Boolean = true) =
            presenter.completed(listOf(CompletionEvent(id, artifact, success)))
    }

    @Test
    fun formatsSelectInitialPhaseAndRapidCapturesKeepLatestFlash() = runTest {
        val f = Fixture(this)
        f.presenter.pressed()
        f.presenter.accepted(1, CaptureOutputFormat.Dng)
        runCurrent()
        assertEquals(CaptureSavePhase.DngProcessing, f.capture(1).phase)
        advanceTimeBy(50)
        f.presenter.pressed()
        f.presenter.accepted(2, CaptureOutputFormat.Jpeg)
        runCurrent()
        assertEquals(CaptureSavePhase.JpegProcessing, f.capture(2).phase)
        assertTrue(f.capture(2).jpegEnabled)
        advanceTimeBy(40)
        runCurrent()
        assertTrue(f.state.value.captureFlash, "The first capture must not clear the second capture's flash")
        advanceTimeBy(50)
        runCurrent()
        assertFalse(f.state.value.captureFlash)
        assertEquals(listOf(1L, 2L), f.state.value.pendingCaptures.map { it.id })
    }

    @Test
    fun dngAndJpegProgressFinishesAndRetiresExactlyOnce() = runTest {
        val f = Fixture(this)
        f.presenter.accepted(1, CaptureOutputFormat.DngAndJpeg)
        f.event(1, Artifact.Dng)
        runCurrent()
        assertEquals(CaptureSavePhase.DngSaved, f.capture(1).phase)
        advanceTimeBy(650)
        runCurrent()
        assertEquals(CaptureSavePhase.JpegProcessing, f.capture(1).phase)
        f.event(1, Artifact.Jpeg)
        assertEquals(CaptureSavePhase.JpegSaved, f.capture(1).phase)
        f.event(1, Artifact.Finished)
        f.event(1, Artifact.Finished)
        runCurrent()
        assertTrue(f.capture(1).terminal)
        advanceTimeBy(1_000)
        runCurrent()
        assertTrue(f.state.value.pendingCaptures.isEmpty())
        assertEquals(1, f.state.value.thumbnailRevision)
    }

    @Test
    fun delayedHandoffAndLateDngCannotOverwriteJpegOrTerminalResult() = runTest {
        val f = Fixture(this)
        f.presenter.accepted(1, CaptureOutputFormat.DngAndJpeg)
        f.event(1, Artifact.Dng)
        f.event(1, Artifact.Jpeg)
        f.event(1, Artifact.Dng, success = false)
        assertEquals(CaptureSavePhase.JpegSaved, f.capture(1).phase)
        f.event(1, Artifact.Finished)
        f.event(1, Artifact.Jpeg, success = false)
        runCurrent()
        advanceTimeBy(650)
        runCurrent()
        assertEquals(CaptureSavePhase.JpegSaved, f.capture(1).phase)
        assertTrue(f.capture(1).terminal)
    }

    @Test
    fun dngOnlyDoesNotEnterJpegAndUnknownEventsDoNotCreateCaptures() = runTest {
        val f = Fixture(this)
        f.presenter.accepted(1, CaptureOutputFormat.Dng)
        f.event(1, Artifact.Dng, success = false)
        f.event(99, Artifact.Dng)
        f.event(99, Artifact.Jpeg)
        f.event(99, Artifact.Finished)
        runCurrent()
        advanceTimeBy(1_000)
        runCurrent()
        assertEquals(CaptureSavePhase.DngFailed, f.capture(1).phase)
        assertEquals(1, f.state.value.pendingCaptures.size)
        assertEquals(0, f.state.value.thumbnailRevision)
    }

    @Test
    fun recoveryRegistrationDoesNotDuplicateOrResetExistingCaptureAndFlash() = runTest {
        val f = Fixture(this)
        f.presenter.pressed()
        f.presenter.accepted(1, CaptureOutputFormat.Jpeg)
        f.event(1, Artifact.Jpeg)
        f.presenter.completed(listOf(CompletionEvent(1, Artifact.Queued, true, jpegRequested = true),
            CompletionEvent(2, Artifact.Queued, true, jpegRequested = false)))
        assertEquals(2, f.state.value.pendingCaptures.size)
        assertEquals(CaptureSavePhase.JpegSaved, f.capture(1).phase)
        assertFalse(f.capture(2).jpegEnabled)
        assertTrue(f.state.value.captureFlash)
    }

    @Test
    fun rejectionUsesIndependentIdsAndRetiresOnlyRejectedCaptures() = runTest {
        val f = Fixture(this)
        f.presenter.accepted(7, CaptureOutputFormat.Dng)
        f.presenter.rejected(true)
        f.presenter.rejected(false)
        runCurrent()
        assertEquals(CaptureSavePhase.JpegFailed, f.capture(-1).phase)
        assertEquals(CaptureSavePhase.DngFailed, f.capture(-2).phase)
        advanceTimeBy(2_000)
        runCurrent()
        assertEquals(listOf(7L), f.state.value.pendingCaptures.map { it.id })
        assertEquals(2, f.state.value.thumbnailRevision)
    }

    @Test
    fun finishedFlagsPreserveDngFailureAndMemoryFallback() = runTest {
        val f = Fixture(this)
        (1L..3L).forEach { f.presenter.accepted(it, CaptureOutputFormat.DngAndJpeg) }
        f.presenter.completed(listOf(
            CompletionEvent(1, Artifact.Finished, false, dngFailed = true),
            CompletionEvent(2, Artifact.Finished, false),
            CompletionEvent(3, Artifact.Finished, true, filmFallbackMemory = true)
        ))
        assertEquals(CaptureSavePhase.DngFailed, f.capture(1).phase)
        assertEquals(CaptureSavePhase.JpegFailed, f.capture(2).phase)
        assertEquals(CaptureSavePhase.DngSaved, f.capture(3).phase)
        assertTrue(f.state.value.pendingCaptures.all { it.terminal })
    }

    @Test
    fun handoffRechecksTerminalStateWhenStateTransformIsRetried() = runTest {
        val state = MutableStateFlow(CaptureUiState(NativeCapabilityProjection.pending()))
        var retryHandoff = false
        val presenter = StillCapturePresentation(backgroundScope) { transform ->
            val candidate = transform(state.value)
            if (retryHandoff && candidate.pendingCaptures.any { it.phase == CaptureSavePhase.JpegProcessing }) {
                retryHandoff = false
                // Another state writer wins the CAS before this handoff can publish.
                state.update { current ->
                    current.copy(pendingCaptures = current.pendingCaptures.map {
                        it.copy(terminal = true, phase = CaptureSavePhase.JpegSaved)
                    })
                }
                state.update(transform)
            } else state.value = candidate
        }
        presenter.accepted(1, CaptureOutputFormat.DngAndJpeg)
        presenter.completed(listOf(CompletionEvent(1, Artifact.Dng, true)))
        retryHandoff = true
        runCurrent()
        advanceTimeBy(650)
        runCurrent()
        val capture = state.value.pendingCaptures.single()
        assertTrue(capture.terminal)
        assertEquals(CaptureSavePhase.JpegSaved, capture.phase)
    }

    @Test
    fun closeCancelsFlashHandoffAndRetirementJobs() = runTest {
        val f = Fixture(this)
        f.presenter.accepted(1, CaptureOutputFormat.DngAndJpeg)
        f.event(1, Artifact.Dng)
        f.presenter.rejected(true)
        runCurrent()
        f.presenter.close()
        val before = f.state.value
        f.presenter.accepted(3, CaptureOutputFormat.Dng)
        f.event(1, Artifact.Finished)
        advanceTimeBy(5_000)
        runCurrent()
        assertEquals(before, f.state.value)
    }
}
