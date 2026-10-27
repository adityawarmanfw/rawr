package com.rawr.camera.integration

import com.rawr.camera.architecture.CaptureTransitions
import com.rawr.camera.integration.StillCaptureCoordinator.CompletionEvent
import com.rawr.camera.integration.StillCaptureCoordinator.CompletionEvent.Artifact
import com.rawr.camera.model.CaptureOutputFormat
import com.rawr.camera.model.CaptureSavePhase
import com.rawr.camera.model.CaptureUiState
import com.rawr.camera.model.PendingCapture
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

/** Save-status/flash presentation only. Admission, native capture and artifact publication live elsewhere. */
internal class StillCapturePresentation(
    parentScope: CoroutineScope,
    private val update: ((CaptureUiState) -> CaptureUiState) -> Unit
) : AutoCloseable {
    private val scope = CoroutineScope(parentScope.coroutineContext + SupervisorJob(parentScope.coroutineContext[Job]))
    private var nextRejectedId = -1L
    private var flashJob: Job? = null
    private var flashGeneration = 0L
    private var closed = false

    @Synchronized
    fun accepted(requestId: Long, format: CaptureOutputFormat) {
        if (closed) return
        register(
            PendingCapture(requestId, format.jpeg,
                if (format.dng) CaptureSavePhase.DngProcessing else CaptureSavePhase.JpegProcessing),
            flash = false
        )
    }

    /** The shutter flash, at the press: acceptance can come seconds later (full-size bursts). */
    @Synchronized
    fun pressed() {
        if (closed) return
        update { CaptureTransitions.setCaptureFlash(it, true) }
        pulseFlash()
    }

    @Synchronized
    fun rejected(jpegEnabled: Boolean) {
        if (closed) return
        val id = nextRejectedId--
        register(
            PendingCapture(id, jpegEnabled,
                if (jpegEnabled) CaptureSavePhase.JpegFailed else CaptureSavePhase.DngFailed),
            flash = false
        )
        retireAfter(id, REJECTION_MS)
    }

    @Synchronized
    fun completed(events: List<CompletionEvent>) {
        if (closed) return
        events.forEach { event ->
            when (event.artifact) {
                Artifact.Queued -> register(PendingCapture(event.requestId, event.jpegRequested), flash = false)
                Artifact.Dng -> {
                    update { current ->
                        val capture = current.pendingCaptures.firstOrNull { it.id == event.requestId }
                        if (capture == null || capture.terminal || capture.phase !in DNG_PHASES) {
                            current
                        } else CaptureTransitions.updateCapturePhase(
                            current, event.requestId,
                            if (event.success) CaptureSavePhase.DngSaved else CaptureSavePhase.DngFailed
                        )
                    }
                    scope.launch {
                        delay(JPEG_HANDOFF_MS)
                        synchronized(this@StillCapturePresentation) {
                            if (!closed) update { current ->
                                val capture = current.pendingCaptures.firstOrNull { it.id == event.requestId }
                                if (capture != null && capture.jpegEnabled && !capture.terminal &&
                                    capture.phase in DNG_RESULTS) {
                                    CaptureTransitions.updateCapturePhase(
                                        current, event.requestId, CaptureSavePhase.JpegProcessing
                                    )
                                } else current
                            }
                        }
                    }
                }
                Artifact.Jpeg -> update { current ->
                    val capture = current.pendingCaptures.firstOrNull { it.id == event.requestId }
                    if (capture == null || capture.terminal) current
                    else CaptureTransitions.updateCapturePhase(
                        current, event.requestId,
                        if (event.success) CaptureSavePhase.JpegSaved else CaptureSavePhase.JpegFailed
                    )
                }
                Artifact.Finished -> {
                    update { current ->
                        current.copy(
                            pendingCaptures = current.pendingCaptures.map { capture ->
                                if (capture.id != event.requestId || capture.terminal) capture else capture.copy(
                                    terminal = true,
                                    phase = when {
                                        event.dngFailed -> CaptureSavePhase.DngFailed
                                        !event.success -> CaptureSavePhase.JpegFailed
                                        event.filmFallbackMemory -> CaptureSavePhase.DngSaved
                                        else -> capture.phase
                                    }
                                )
                            }
                        )
                    }
                    retireAfter(event.requestId, FINISHED_MS)
                }
            }
        }
    }

    private fun register(capture: PendingCapture, flash: Boolean) {
        update { current ->
            if (current.pendingCaptures.any { it.id == capture.id }) current
            else current.copy(
                pendingCaptures = current.pendingCaptures + capture,
                captureFlash = current.captureFlash || flash
            )
        }
    }

    private fun pulseFlash() {
        flashJob?.cancel()
        val generation = ++flashGeneration
        flashJob = scope.launch {
            delay(FLASH_MS)
            synchronized(this@StillCapturePresentation) {
                if (!closed && generation == flashGeneration) update { CaptureTransitions.setCaptureFlash(it, false) }
            }
        }
    }

    private fun retireAfter(id: Long, delayMs: Long) {
        scope.launch {
            delay(delayMs)
            synchronized(this@StillCapturePresentation) {
                if (!closed) update { CaptureTransitions.completeCapture(it, id) }
            }
        }
    }

    @Synchronized
    override fun close() {
        closed = true
        scope.cancel()
    }

    private companion object {
        const val FLASH_MS = 90L
        const val JPEG_HANDOFF_MS = 650L
        const val FINISHED_MS = 1_000L
        const val REJECTION_MS = 2_000L
        val DNG_RESULTS = setOf(CaptureSavePhase.DngSaved, CaptureSavePhase.DngFailed)
        val DNG_PHASES = DNG_RESULTS + CaptureSavePhase.DngProcessing
    }
}
