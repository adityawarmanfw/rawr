package com.rawr.camera.integration

import com.rawr.camera.architecture.CaptureReducer
import com.rawr.camera.architecture.CaptureTransitions
import com.rawr.camera.architecture.StartSelfTimerCountdown
import com.rawr.camera.architecture.TickSelfTimer
import com.rawr.camera.model.CaptureUiState
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch

/** Owns the photo shutter countdown job; presentation state remains in the shared reducer. */
internal class CaptureSelfTimer(
    private val scope: CoroutineScope,
    private val state: StateFlow<CaptureUiState>,
    private val update: ((CaptureUiState) -> CaptureUiState) -> Unit,
    private val onExpired: (Any) -> Unit,
    private val foregroundToken: () -> Any?
) : AutoCloseable {
    private var job: Job? = null
    private var activeRunId: Long? = null
    private var closed = false

    /** The press binds its foreground identity; a countdown carries it to expiry. */
    @Synchronized
    fun trigger() {
        if (closed) return
        if (CaptureTransitions.isSelfTimerCounting(state.value)) {
            cancel()
            return
        }
        val token = foregroundToken() ?: return
        if (state.value.selfTimer.seconds <= 0) {
            onExpired(token)
            return
        }
        update { CaptureReducer.reduce(it, StartSelfTimerCountdown) }
        val current = state.value
        if (current.selfTimerRemainingMs == null) return
        stopJob()
        activeRunId = current.selfTimerRunId
        job = scope.launch {
            while (true) {
                delay(TICK_MS)
                if (!tick(current.selfTimerRunId, token)) return@launch
            }
        }
    }

    /** Reducer/context changes invalidate runs; no-op preference actions retain their ticker. */
    @Synchronized
    fun reconcile() {
        val current = state.value
        if (current.selfTimerRemainingMs == null || current.selfTimerRunId != activeRunId) stopJob()
    }

    @Synchronized
    fun cancel() {
        update(CaptureTransitions::cancelSelfTimerRun)
        stopJob()
    }

    @Synchronized
    private fun tick(runId: Long, token: Any): Boolean {
        if (closed || activeRunId != runId) return false
        val current = state.value
        val remaining = current.selfTimerRemainingMs ?: return false
        if (current.selfTimerRunId != runId) return false
        val next = remaining - TICK_MS
        update { CaptureReducer.reduce(it, TickSelfTimer(next, runId)) }
        if (next > 0) return true
        activeRunId = null
        job = null
        // Cancellation or a replacement run can race the state transform.
        if (state.value.selfTimerRunId == runId && state.value.selfTimerRemainingMs == null) onExpired(token)
        return false
    }

    private fun stopJob() {
        job?.cancel()
        job = null
        activeRunId = null
    }

    @Synchronized
    override fun close() {
        closed = true
        cancel()
    }

    private companion object {
        const val TICK_MS = 100L
    }
}
