package com.rawr.camera.fixtures

import com.rawr.camera.architecture.ActivateSpotAe
import com.rawr.camera.architecture.ApplicationCaptureAction
import com.rawr.camera.architecture.ApplyCameraCapabilities
import com.rawr.camera.architecture.CancelSpotAe
import com.rawr.camera.architecture.CaptureAction
import com.rawr.camera.architecture.CaptureReducer
import com.rawr.camera.architecture.CaptureScreenController
import com.rawr.camera.architecture.CaptureTransitions
import com.rawr.camera.architecture.ClearAutofocus
import com.rawr.camera.architecture.FinishSpotAeMove
import com.rawr.camera.architecture.FocusAt
import com.rawr.camera.architecture.MoveSpotAe
import com.rawr.camera.architecture.PresentationCaptureAction
import com.rawr.camera.architecture.Refocus
import com.rawr.camera.architecture.ScrubRenderExposure
import com.rawr.camera.architecture.ScrubTone
import com.rawr.camera.architecture.SelectLens
import com.rawr.camera.architecture.SetExposureCandidate
import com.rawr.camera.architecture.SetExposureMode
import com.rawr.camera.architecture.SetFocusMode
import com.rawr.camera.architecture.SetManualFocus
import com.rawr.camera.architecture.CancelSelfTimer
import com.rawr.camera.architecture.CycleSelfTimer
import com.rawr.camera.architecture.LockWhiteBalance
import com.rawr.camera.architecture.SetCaptureMode
import com.rawr.camera.architecture.SetCaptureSelfTimer
import com.rawr.camera.architecture.StartSelfTimerCountdown
import com.rawr.camera.architecture.TickSelfTimer
import com.rawr.camera.architecture.SetWhiteBalanceMode
import com.rawr.camera.architecture.SetWhiteBalanceTempTint
import com.rawr.camera.architecture.SyncToneControls
import com.rawr.camera.architecture.TriggerCapture
import com.rawr.camera.model.CameraCapabilities
import com.rawr.camera.model.CaptureUiState
import com.rawr.camera.model.ExposureMode
import com.rawr.camera.model.FocusMode
import com.rawr.camera.model.NormalizedPoint
import com.rawr.camera.model.PendingCapture
import com.rawr.camera.model.TargetStatus
import java.util.concurrent.atomic.AtomicLong
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

/**
 * Deterministic, in-memory application adapter used by previews and tests.
 *
 * Production-facing commands are deliberately routed through this controller instead of the
 * presentation reducer. This adapter applies most request/result pairs immediately while
 * preserving the production requested-vs-applied architecture.
 */
class InMemoryCaptureScreenController(
    capabilities: CameraCapabilities,
    initialSelectedLensId: String = capabilities.lenses.first().id,
    private val timings: CaptureSimulationTimings = CaptureSimulationTimings(),
    dispatcher: CoroutineDispatcher = Dispatchers.Default
) : CaptureScreenController {
    private val mutableState =
        MutableStateFlow(
            CaptureUiState(
                capabilities = capabilities,
                selectedLensId =
                    initialSelectedLensId.also { lensId ->
                        require(
                            capabilities.lenses.any { it.id == lensId }
                        ) { "Initial lens must exist in capabilities" }
                    }
            )
        )
    override val state: StateFlow<CaptureUiState> = mutableState.asStateFlow()

    private val scope = CoroutineScope(SupervisorJob() + dispatcher)
    private val captureIds = AtomicLong(0)
    private var afJob: Job? = null
    private var afDismissJob: Job? = null
    private var aeJob: Job? = null
    private var captureFlashJob: Job? = null
    private var selfTimerJob: Job? = null

    override fun dispatch(action: CaptureAction) {
        when (action) {
            is PresentationCaptureAction -> {
                update { CaptureReducer.reduce(it, action) }
                if (action is CancelSelfTimer || action is CycleSelfTimer ||
                    action is SetCaptureSelfTimer || action is SetCaptureMode
                ) {
                    selfTimerJob?.cancel()
                    selfTimerJob = null
                }
            }
            is ApplicationCaptureAction -> dispatchApplicationAction(action)
        }
    }

    override fun close() {
        selfTimerJob?.cancel()
        selfTimerJob = null
        scope.cancel()
    }

    private fun dispatchApplicationAction(action: ApplicationCaptureAction) {
        when (action) {
            is SetExposureMode -> {
                update { CaptureTransitions.requestExposureMode(it, action.mode) }
            }

            is SetWhiteBalanceMode -> {
                update { CaptureTransitions.requestWhiteBalanceMode(it, action.mode) }
            }

            is SetWhiteBalanceTempTint -> {
                update { CaptureTransitions.setWhiteBalanceTempTint(it, action.temperatureK, action.tint) }
            }

            is LockWhiteBalance -> {
                update { CaptureTransitions.lockWhiteBalance(it, action.temperatureK, action.tint) }
            }

            is SetExposureCandidate -> {
                requestExposureCandidate(action)
            }

            is ScrubTone -> {
                update { CaptureTransitions.scrubTone(it, action.target, action.delta) }
            }

            is ScrubRenderExposure -> {
                update { CaptureTransitions.scrubRenderExposure(it, action.deltaTenths) }
            }

            is SyncToneControls -> {
                update {
                    it.copy(
                        blacks = action.blacks.coerceIn(-100, 100),
                        shadows = action.shadows.coerceIn(-100, 100),
                        contrast = action.contrast.coerceIn(-100, 100),
                        midtones = action.midtones.coerceIn(-100, 100),
                        highlights = action.highlights.coerceIn(-100, 100),
                        whites = action.whites.coerceIn(-100, 100),
                        saturation = action.saturation.coerceIn(-100, 100),
                        vibrance = action.vibrance.coerceIn(-100, 100),
                        renderExposureTenths = action.renderExposureTenths
                    )
                }
            }

            is SelectLens -> {
                update { CaptureTransitions.selectLens(it, action.id) }
            }

            is SetManualFocus -> {
                update { CaptureTransitions.setManualFocus(it, action.normalized) }
            }

            is ApplyCameraCapabilities -> {
                applyCapabilities(action.capabilities)
            }

            is FocusAt -> {
                beginAf(action.point)
            }

            Refocus -> {
                beginAf(state.value.focus.target)
            }

            ClearAutofocus -> {
                afJob?.cancel()
                afJob = null
                afDismissJob?.cancel()
                afDismissJob = null
                update(CaptureTransitions::clearAutofocus)
            }

            is SetFocusMode -> {
                setFocusMode(action.mode)
            }

            is ActivateSpotAe -> {
                if (state.value.exposureControl.mode == ExposureMode.Auto) beginSpotAe(action.point)
            }

            is MoveSpotAe -> {
                if (state.value.exposureControl.mode == ExposureMode.Auto &&
                    state.value.spotAe.active
                ) {
                    moveSpotAe(action.point)
                }
            }

            FinishSpotAeMove -> {
                if (state.value.exposureControl.mode == ExposureMode.Auto &&
                    state.value.spotAe.active
                ) {
                    beginSpotAe(state.value.spotAe.target)
                }
            }

            CancelSpotAe -> {
                aeJob?.cancel()
                aeJob = null
                update(CaptureTransitions::cancelSpotAe)
            }

            TriggerCapture -> {
                triggerCaptureOrSelfTimer()
            }
        }
    }

    private fun applyCapabilities(capabilities: CameraCapabilities) {
        // Any in-flight AF/AE result belongs to the previous capability/camera context.
        afJob?.cancel()
        afJob = null
        afDismissJob?.cancel()
        afDismissJob = null
        aeJob?.cancel()
        aeJob = null
        selfTimerJob?.cancel()
        selfTimerJob = null
        update {
            CaptureTransitions.cancelSelfTimerRun(
                CaptureTransitions.applyCameraCapabilities(it, capabilities)
            )
        }
    }

    private fun requestExposureCandidate(action: SetExposureCandidate) {
        update { current ->
            val requested =
                CaptureTransitions.requestExposureCandidate(
                    current,
                    action.parameter,
                    action.candidateId
                )
            // The in-memory backend acknowledges immediately. A device-backed controller may leave these
            // states divergent until Camera2/AE reports what was actually applied.
            if (requested === current) {
                current
            } else {
                CaptureTransitions.reportAppliedExposureCandidate(
                    requested,
                    requested.cameraContextGeneration,
                    action.parameter,
                    action.candidateId
                )
            }
        }
    }

    private fun beginAf(point: NormalizedPoint) {
        if (!state.value.capabilities.tapAfSupported) return
        afJob?.cancel()
        afDismissJob?.cancel()
        update { CaptureTransitions.beginAutofocus(it, point) }
        val contextGeneration = state.value.cameraContextGeneration
        val inAf = state.value.focus.mode == FocusMode.Af
        afJob =
            scope.launch {
                delay(timings.settleDelayMs)
                update { CaptureTransitions.completeAutofocus(it, contextGeneration, TargetStatus.Settled) }
            }
        // Transient tap box in full auto dismisses back to continuous AF.
        if (inAf) {
            afDismissJob =
                scope.launch {
                    delay(AF_AUTO_DISMISS_MS)
                    if (state.value.focus.mode == FocusMode.Af) {
                        afJob?.cancel()
                        afJob = null
                        update(CaptureTransitions::clearAutofocus)
                    }
                }
        }
    }

    private fun setFocusMode(mode: FocusMode) {
        val before = state.value
        if (mode == FocusMode.Mf && !before.capabilities.manualFocus.supported) return

        afJob?.cancel()
        afJob = null
        afDismissJob?.cancel()
        afDismissJob = null
        update { CaptureTransitions.selectFocusMode(it, mode) }
        // Full auto shows no box; AfLock re-locks at the current target like production.
        if (mode == FocusMode.AfLock) beginAf(state.value.focus.target)
    }

    private fun beginSpotAe(point: NormalizedPoint) {
        aeJob?.cancel()
        update { CaptureTransitions.beginSpotAe(it, point) }
        val contextGeneration = state.value.cameraContextGeneration
        aeJob =
            scope.launch {
                delay(timings.settleDelayMs)
                update { CaptureTransitions.completeSpotAe(it, contextGeneration, TargetStatus.Settled) }
            }
    }

    private fun moveSpotAe(point: NormalizedPoint) {
        aeJob?.cancel()
        aeJob = null
        update { CaptureTransitions.moveSpotAe(it, point) }
    }

    private fun triggerCaptureOrSelfTimer() {
        if (CaptureTransitions.isSelfTimerCounting(state.value)) {
            update(CaptureTransitions::cancelSelfTimerRun)
            selfTimerJob?.cancel()
            selfTimerJob = null
            return
        }
        if (state.value.selfTimer.seconds > 0) {
            update { CaptureReducer.reduce(it, StartSelfTimerCountdown) }
            val runId = state.value.selfTimerRunId
            if (state.value.selfTimerRemainingMs == null) return
            selfTimerJob?.cancel()
            selfTimerJob = scope.launch {
                while (true) {
                    delay(SELF_TIMER_TICK_MS)
                    val remaining = state.value.selfTimerRemainingMs ?: return@launch
                    if (state.value.selfTimerRunId != runId) return@launch
                    val next = remaining - SELF_TIMER_TICK_MS
                    update { CaptureReducer.reduce(it, TickSelfTimer(next, runId)) }
                    if (next <= 0L) break
                }
                if (state.value.selfTimerRunId == runId) beginCapture()
            }
            return
        }
        beginCapture()
    }

    private fun beginCapture() {
        val capture = PendingCapture(captureIds.incrementAndGet(), state.value.jpegEnabled)
        update { CaptureTransitions.enqueueCapture(it, capture) }

        captureFlashJob?.cancel()
        captureFlashJob =
            scope.launch {
                delay(timings.captureFlashMs)
                update { CaptureTransitions.setCaptureFlash(it, false) }
            }

        scope.launch {
            delay(timings.captureProcessingBaseMs + (capture.id % 3) * timings.captureProcessingStaggerMs)
            update { CaptureTransitions.completeCapture(it, capture.id) }
        }
    }

    private inline fun update(transform: (CaptureUiState) -> CaptureUiState) {
        while (true) {
            val current = mutableState.value
            val next = transform(current)
            if (next === current || mutableState.compareAndSet(current, next)) return
        }
    }

    private companion object {
        const val AF_AUTO_DISMISS_MS = 4000L
        const val SELF_TIMER_TICK_MS = 100L
    }
}
