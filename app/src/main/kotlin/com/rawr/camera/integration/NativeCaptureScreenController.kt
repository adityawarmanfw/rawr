package com.rawr.camera.integration

import com.rawr.camera.architecture.*
import com.rawr.camera.model.*
import com.rawr.camera.model.TonemapControlContract
import kotlinx.coroutines.*
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

/**
 * Production capture-screen application adapter.
 *
 * Kotlin owns only UI/application state and semantic dispatch. Camera legality, Camera2
 * request/result state, AF regions and manual controls are native C++ responsibilities.
 */
class NativeCaptureScreenController(
    private val coordinator: RawPreviewCoordinator,
    private val requestStillCapture: (CaptureOutputFormat) -> Long,
    private val onCaptureQueued: (CaptureOutputFormat) -> Unit = {},
    private val pollStillCompletion: () -> List<StillCaptureCoordinator.CompletionEvent>,
    private val onToneScrubbed: (ToneParameter, Int) -> Unit = { _, _ -> },
    private val onRenderExposureScrubbed: (Int) -> Unit = {},
    private val onCapturePreferencesChanged: (CaptureUiState, CaptureUiState) -> Unit = { _, _ -> },
    private val videoLocked: () -> Boolean = { false },
    private val onRawCpuIngress: () -> Unit = {},
    /** A lens switch was accepted (persisted as the next launch's lens). */
    private val onLensSelected: (String) -> Unit = {},
    dispatcher: CoroutineDispatcher = Dispatchers.Default
) : CaptureScreenController {
    private val mutableState =
        MutableStateFlow(
            CaptureUiState(
                capabilities = NativeCapabilityProjection.pending(),
                // Empty until native reports the lens it opened.
                selectedLensId = "",
                exposureApplied = ExposureAppliedState.pending(),
                focus = FocusUiState(status = TargetStatus.Hidden)
            )
        )
    override val state: StateFlow<CaptureUiState> = mutableState.asStateFlow()

    private val scope = CoroutineScope(SupervisorJob() + dispatcher)
    private val captureQueue = StillCaptureDispatchQueue(scope, onCaptureQueued, ::captureStill)
    private val overlayTriggers = OverlayTriggerManager(scope, coordinator::setMonitoringOverlay)
    private val focusRequestIds = java.util.concurrent.atomic.AtomicLong()
    private val snapshotPresentation = NativeCaptureSnapshotPresentation()
    private val whiteBalanceRequestIds = java.util.concurrent.atomic.AtomicLong()
    private val capturePresentation = StillCapturePresentation(scope, ::update)
    private val selfTimer = CaptureSelfTimer(scope, state, ::update, ::enqueueStillCapture)
    // Once per process: the CPU path is a device/stream property, not a per-frame event.
    private var rawCpuIngressNotified = false

    init {
        // Native starts on the first lens of its profile; the snapshot's
        // profile lenses then reconcile selectedLensId.
        scope.launch {
            while (isActive) {
                val snapshot = NativeCameraUiSnapshot.parse(coordinator.cameraControlSnapshot())
                if (snapshot != null) {
                    if (snapshot.rawCpuIngress && !rawCpuIngressNotified) {
                        rawCpuIngressNotified = true
                        onRawCpuIngress()
                    }
                    applyNativeSnapshot(snapshot)
                    val focus = state.value.focus
                    overlayTriggers.autofocusStatus(focus.status, focus.mode == FocusMode.Mf)
                }
                updateFaces(NativeCameraUiSnapshot.parseFaces(coordinator.faceDetectionsSnapshot()))
                captureQueue.poll { capturePresentation.completed(pollStillCompletion()) }
                // Applied exposure is display telemetry, not part of the AE loop.
                // 10 Hz is responsive enough and avoids unnecessary JNI/JSON/UI churn.
                delay(100)
            }
        }
    }

    override fun dispatch(action: CaptureAction) {
        if (videoLocked() && (action is SetCaptureMode || action is ToggleVideoLog ||
                action is CycleVideoResolution || action is CycleVideoFps)) return
        if ((action is ScrubTone || action is ScrubRenderExposure) &&
            state.value.captureMode == CaptureMode.Video && state.value.videoLogEnabled) return
        when (action) {
            is PresentationCaptureAction -> {
                val before = state.value
                update { CaptureReducer.reduce(it, action) }
                if (action is ToggleOverlayArmed || action is ToggleFalseColor ||
                    action is RestoreCapturePreferences
                ) {
                    syncOverlayArming()
                }
                selfTimer.reconcile()
                if (action is SetCaptureMode || action is CycleVideoFps || action is RestoreCapturePreferences) {
                    val current = state.value
                    coordinator.setVideoMode(current.captureMode == CaptureMode.Video, current.videoFps)
                }
                if (action !is RestoreCapturePreferences && action.isDurableCapturePreference()) {
                    onCapturePreferencesChanged(before, state.value)
                }
            }

            is ApplicationCaptureAction -> {
                dispatchApplication(action)
            }
        }
    }

    private fun dispatchApplication(action: ApplicationCaptureAction) {
        when (action) {
            is SetExposureMode -> {
                setExposureMode(action.mode)
            }

            is SetWhiteBalanceMode -> {
                setWhiteBalanceMode(action.mode)
            }

            is SetWhiteBalanceTempTint -> {
                setWhiteBalanceTempTint(action.temperatureK, action.tint)
            }

            is LockWhiteBalance -> {
                lockWhiteBalance(action.temperatureK, action.tint)
            }

            is SetExposureCandidate -> {
                setExposureCandidate(action)
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
                        renderExposureTenths = action.renderExposureTenths.coerceIn(
                            TonemapControlContract.EXPOSURE_MIN_TENTHS,
                            TonemapControlContract.EXPOSURE_MAX_TENTHS
                        )
                    )
                }
            }

            is ScrubRenderExposure -> {
                val next = CaptureTransitions.scrubRenderExposure(state.value, action.deltaTenths)
                mutableState.value = next
                // Persisted by CaptureViewModel; the settings publish applies the
                // complete ImageToneState to TonemapEngine.
                onRenderExposureScrubbed(next.renderExposureTenths)
            }

            is ScrubTone -> {
                val next = CaptureTransitions.scrubTone(state.value, action.target, action.delta)
                mutableState.value = next
                val value = next.toneValueFor(action.target)
                // Preserve the low-latency native shortcut for the two original quick controls.
                // All parameters are persisted/published by CaptureViewModel, which applies the
                // complete ImageToneState atomically to TonemapEngine.
                when (action.target) {
                    ToneParameter.Shadows -> {
                        coordinator.setQuickToneNativeValue(
                            0,
                            TonemapControlContract.shadowNativeFromUi(value.toFloat())
                        )
                    }

                    ToneParameter.Highlights -> {
                        coordinator.setQuickToneNativeValue(
                            1,
                            TonemapControlContract.highlightNativeFromUi(value.toFloat())
                        )
                    }

                    else -> {
                        Unit
                    }
                }
                onToneScrubbed(action.target, value)
                if (action.target == ToneParameter.Shadows || action.target == ToneParameter.Blacks) {
                    overlayTriggers.pulseShadow()
                }
            }

            is SelectLens -> {
                selectLens(action.id)
            }

            is SetManualFocus -> {
                setManualFocus(action.normalized)
            }

            is ApplyCameraCapabilities -> {
                Unit
            }

            // Native snapshots are the only production capability authority.
            is FocusAt -> {
                focusAt(action.point)
            }

            Refocus -> {
                focusAt(state.value.focus.target)
            }

            ClearAutofocus -> {
                clearAutofocus()
            }

            is SetFocusMode -> {
                setFocusMode(action.mode)
            }

            is ActivateSpotAe -> {
                if (state.value.exposureControl.mode == ExposureMode.Auto) {
                    update { CaptureTransitions.beginSpotAe(it, action.point) }
                    coordinator.setSpotAeTarget(true, action.point.x, action.point.y)
                }
            }

            is MoveSpotAe -> {
                if (state.value.exposureControl.mode == ExposureMode.Auto && state.value.spotAe.active) {
                    update { CaptureTransitions.moveSpotAe(it, action.point) }
                    coordinator.setSpotAeTarget(true, action.point.x, action.point.y)
                }
            }

            FinishSpotAeMove -> {
                if (state.value.exposureControl.mode == ExposureMode.Auto && state.value.spotAe.active) {
                    val target = state.value.spotAe.target
                    coordinator.setSpotAeTarget(true, target.x, target.y)
                }
            }

            CancelSpotAe -> {
                update(CaptureTransitions::cancelSpotAe)
                coordinator.setSpotAeTarget(false, 0.5f, 0.5f)
            }

            TriggerCapture -> {
                selfTimer.trigger()
            }
        }
    }

    private fun PresentationCaptureAction.isDurableCapturePreference(): Boolean = when (this) {
        CycleOutputFormat, ToggleWaveformMode -> true
        is ToggleOverlayArmed, is ToggleFalseColor, is ToggleScope -> true
        ToggleFilmSim, ToggleMultiframe -> true
        is SetCaptureLayout -> true
        is SetCaptureMode, CycleVideoResolution, CycleVideoFps, ToggleVideoLog -> true
        CycleSelfTimer, is SetCaptureSelfTimer -> true
        else -> false
    }

    private fun syncOverlayArming() {
        overlayTriggers.setArmed(state.value.armedOverlays)
        overlayTriggers.setFalseColor(state.value.falseColorManual)
    }

    private fun setExposureMode(mode: ExposureMode) {
        if (mode !in state.value.capabilities.supportedExposureModes) return
        // Native camera state is authoritative for mode acceptance. The next
        // snapshot performs the UI transition only after Camera2/RAW-AE ownership
        // accepted the requested semantic mode.
        coordinator.setExposureMode(mode.ordinal)
    }

    private fun setWhiteBalanceMode(mode: WhiteBalanceMode) {
        val current = state.value
        val next = CaptureTransitions.requestWhiteBalanceMode(current, mode)
        if (next == current) return
        val requestId = whiteBalanceRequestIds.incrementAndGet()
        update {
            if (mode == WhiteBalanceMode.ManualTempTint) CaptureTransitions.requestWhiteBalanceMode(it, mode)
            else CaptureTransitions.snapWhiteBalanceDisplayToMode(it, mode, it.whiteBalanceTemperatureK, it.whiteBalanceTint)
        }
        coordinator.setWhiteBalanceMode(mode.awbValue, requestId)
    }

    private fun setWhiteBalanceTempTint(temperatureK: Int, tint: Int) {
        val current = state.value
        val next = CaptureTransitions.setWhiteBalanceTempTint(current, temperatureK, tint)
        if (next == current) return
        // Report gesture intent against what the user saw, not the native
        // request coordinate. Native seeds the untouched axis from live AWB.
        val editedAxes = if (current.whiteBalanceMode == WhiteBalanceMode.ManualTempTint) 3 else
            (if (temperatureK != current.whiteBalanceTemperatureK) 1 else 0) or
                (if (tint != current.whiteBalanceTint) 2 else 0)
        val requestId = whiteBalanceRequestIds.incrementAndGet()
        update { CaptureTransitions.setWhiteBalanceTempTint(it, temperatureK, tint) }
        coordinator.setWhiteBalanceTempTint(temperatureK, tint, editedAxes, requestId)
    }

    private fun lockWhiteBalance(temperatureK: Int, tint: Int) {
        val current = state.value
        val next = CaptureTransitions.lockWhiteBalance(current, temperatureK, tint)
        if (next == current) return
        val requestId = whiteBalanceRequestIds.incrementAndGet()
        update { CaptureTransitions.lockWhiteBalance(it, temperatureK, tint) }
        coordinator.setWhiteBalanceLocked(temperatureK, tint, requestId)
    }

    private fun setExposureCandidate(action: SetExposureCandidate) {
        val p = snapshotPresentation.exposureProjectionFor(state.value) ?: return
        when (action.parameter) {
            ExposureParameter.Iso -> {
                p.isoValues[action.candidateId]?.let(coordinator::setManualSensitivity)
            }

            ExposureParameter.Shutter -> {
                p.shutterAnglesDeg[action.candidateId]?.let(coordinator::setShutterAngleDegrees)
                    ?: p.shutterValuesNs[action.candidateId]?.let(coordinator::setManualExposureTimeNs)
            }

            ExposureParameter.Ev -> {
                p.evSteps[action.candidateId]?.let(coordinator::setExposureCompensationSteps)
            }
        } ?: return
        update { CaptureTransitions.requestExposureCandidate(it, action.parameter, action.candidateId) }
        overlayTriggers.pulseRawHighlight()
    }

    private fun selectLens(lensId: String) {
        if (state.value.capabilities.lenses
                .none { it.id == lensId }
        ) {
            return
        }
        snapshotPresentation.invalidate()
        // Invalidate pending gestures and acknowledge their clear before the
        // new lens is selected. Older tap snapshots cannot reopen its box.
        coordinator.clearFocus(focusRequestIds.incrementAndGet())
        selfTimer.cancel()
        update { current ->
            val pending = CaptureTransitions.applyCameraCapabilities(
                current, NativeCapabilityProjection.pending(current.capabilities.lenses)
            )
            CaptureTransitions.selectLens(pending, lensId).copy(
                exposureApplied = ExposureAppliedState.pending(),
                sensitivityBoost = null,
                rawFps = null,
                viewfinderFps = null,
                faceDetections = emptyList(),
                focus =
                    pending.focus.copy(
                        status = TargetStatus.Hidden,
                        appliedFocusDiopters = null,
                        backendNativeReadout = null
                    )
            )
        }
        coordinator.selectLens(lensId)
        onLensSelected(lensId)
    }

    private fun focusAt(point: NormalizedPoint) {
        if (!state.value.capabilities.tapAfSupported) return
        val p = point.clamped(0f)
        val requestId = focusRequestIds.incrementAndGet()
        update { CaptureTransitions.beginAutofocus(it, p) }
        overlayTriggers.autofocusStart()
        coordinator.focusAt(p.x, p.y, requestId)
    }

    /**
     * Back to full auto (retap on the box / auto-dismiss / Af entry): hides
     * the box and drops the native tap region + trigger so continuous AF
     * meters the full frame again.
     */
    private fun clearAutofocus() {
        val requestId = focusRequestIds.incrementAndGet()
        update(CaptureTransitions::clearAutofocus)
        overlayTriggers.autofocusCancel()
        coordinator.clearFocus(requestId)
    }

    /** Face boxes are ephemeral display data: replace when changed, never accumulate. */
    private fun updateFaces(faces: List<FaceDetection>) {
        if (state.value.faceDetections != faces) {
            update { it.copy(faceDetections = faces) }
        }
    }

    private fun setFocusMode(mode: FocusMode) {
        val state = state.value
        if (mode == FocusMode.Mf && !state.capabilities.manualFocus.supported) return
        if (mode == FocusMode.AfLock && !state.capabilities.tapAfSupported) return
        val requestId = focusRequestIds.incrementAndGet()
        val nativeMode =
            when (mode) {
                FocusMode.Af -> 0
                FocusMode.AfLock -> 1
                FocusMode.Mf -> 2
            }
        val target = state.focus.target.clamped(0f)
        coordinator.setFocusMode(nativeMode, target.x, target.y, requestId)
        if (mode == FocusMode.AfLock) overlayTriggers.autofocusStart()
        update { NativeFocusPresentation.requestMode(it, mode) }
    }

    private fun setManualFocus(normalized: Float) {
        if (!state.value.capabilities.manualFocus.supported) return
        val requestId = focusRequestIds.incrementAndGet()
        update { NativeFocusPresentation.requestManualFocus(it, normalized) }
        coordinator.setManualFocusNormalized(normalized, requestId)
        overlayTriggers.pulsePeakManual()
    }

    private fun enqueueStillCapture() {
        val format = CaptureOutputFormat.from(state.value.dngEnabled, state.value.jpegEnabled)
        try {
            captureQueue.enqueue(format)
        } catch (error: RuntimeException) {
            android.util.Log.e("RawrCamCapture", "Could not start photo processing", error)
            capturePresentation.rejected(format.jpeg)
        }
    }

    private fun captureStill(format: CaptureOutputFormat) {
        val nativeRequestId = try {
            requestStillCapture(format)
        } catch (error: RuntimeException) {
            android.util.Log.e("RawrCamCapture", "Capture preparation failed", error)
            0L
        }
        if (nativeRequestId == 0L) {
            capturePresentation.rejected(format.jpeg)
            return
        }
        capturePresentation.accepted(nativeRequestId, format)
    }

    private fun applyNativeSnapshot(snapshot: NativeCameraUiSnapshot) {
        val prepared = snapshotPresentation.prepare(state.value, snapshot) ?: return
        update { current ->
            prepared.project(current, focusRequestIds.get(), whiteBalanceRequestIds.get())
        }
        selfTimer.reconcile()
    }

    private inline fun update(transform: (CaptureUiState) -> CaptureUiState) {
        while (true) {
            val current = mutableState.value
            val next = transform(current)
            if (next === current || mutableState.compareAndSet(current, next)) return
        }
    }

    override fun close() {
        selfTimer.close()
        capturePresentation.close()
        scope.cancel()
    }
}
