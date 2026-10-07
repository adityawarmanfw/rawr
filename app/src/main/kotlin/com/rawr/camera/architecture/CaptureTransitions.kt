package com.rawr.camera.architecture

import com.rawr.camera.model.*

/**
 * Pure state transitions around application/backend commands.
 * Controllers own I/O/timing/cancellation; this object owns the capture-screen meaning of requests/results.
 */
object CaptureTransitions {
    /**
     * Atomically installs a new camera-context capability snapshot and reconciles state expressed
     * in capability identity space. Any transient AF/Spot-AE result and applied-exposure readout
     * belongs to the replaced camera context and is invalidated here.
     *
     * Because exposure candidate ids are opaque, an invalid requested id cannot be numerically
     * clamped here without inventing backend semantics. The new capability's explicit initial
     * candidate is therefore the safe fallback.
     */
    fun applyCameraCapabilities(state: CaptureUiState, capabilities: CameraCapabilities): CaptureUiState {
        fun reconcileRequested(currentId: String, capability: DiscreteExposureCapability): String =
            if (capability.candidate(currentId) != null) currentId else capability.initialCandidateId

        val reconciledFocusMode =
            when {
                state.focus.mode == FocusMode.Mf && !capabilities.manualFocus.supported -> FocusMode.Af
                state.focus.mode == FocusMode.AfLock -> FocusMode.Af
                else -> state.focus.mode
            }
        val reconciledFocus =
            state.focus.copy(
                mode = reconciledFocusMode,
                status = TargetStatus.Hidden,
                selectorOpen = false,
                mfNormalized =
                    state.focus.mfNormalized.coerceIn(
                        capabilities.manualFocus.minNormalized,
                        capabilities.manualFocus.maxNormalized
                    ),
                // A readout belongs to the camera that produced it; do not carry it across a
                // capability/camera replacement without a fresh backend report.
                backendNativeReadout = null,
                appliedFocusDiopters = null
            )

        val selectedLensId =
            state.selectedLensId
                .takeIf { id -> capabilities.lenses.any { it.id == id } }
                ?: capabilities.lenses.first().id

        // White-balance intent survives a camera change only when the new
        // context supports it; temperature/tint are device-independent and
        // carry over (clamped) so manual tuning is not lost on lens switch.
        val reconciledWhiteBalanceMode =
            when {
                state.whiteBalanceMode == WhiteBalanceMode.Auto -> WhiteBalanceMode.Auto
                state.whiteBalanceMode == WhiteBalanceMode.ManualTempTint &&
                    capabilities.manualWhiteBalanceSupported -> WhiteBalanceMode.ManualTempTint
                state.whiteBalanceMode in capabilities.supportedWhiteBalanceModes -> state.whiteBalanceMode
                else -> WhiteBalanceMode.Auto
            }

        return state.copy(
            capabilities = capabilities,
            cameraContextGeneration = state.cameraContextGeneration + 1L,
            exposureControl =
                state.exposureControl.copy(
                    requestedShutterId =
                        reconcileRequested(
                            state.exposureControl.requestedShutterId,
                            capabilities.shutter
                        ),
                    requestedIsoId =
                        reconcileRequested(
                            state.exposureControl.requestedIsoId,
                            capabilities.iso
                        ),
                    requestedEvId =
                        reconcileRequested(
                            state.exposureControl.requestedEvId,
                            capabilities.ev
                        )
                ),
            // Values reported by the replaced camera context are not valid observations for the
            // new context. Keep the monitor unavailable until fresh backend reports arrive.
            exposureApplied = ExposureAppliedState.pending(),
            sensitivityBoost = null,
            rawFps = null,
            viewfinderFps = null,
            selectedLensId = selectedLensId,
            whiteBalanceMode = reconciledWhiteBalanceMode,
            whiteBalanceTemperatureK =
                state.whiteBalanceTemperatureK.coerceIn(
                    WhiteBalanceMode.TEMP_MIN_K,
                    WhiteBalanceMode.TEMP_MAX_K
                ),
            whiteBalanceTint = state.whiteBalanceTint.coerceIn(WhiteBalanceMode.TINT_MIN, WhiteBalanceMode.TINT_MAX),
            // The native WB estimate belongs to the replaced camera context; drop the
            // seed until fresh backend reports arrive (mirrors applied-focus).
            autoWhiteBalanceTemperatureK = null,
            autoWhiteBalanceTint = null,
            focus = reconciledFocus,
            // Face boxes belong to the camera that produced them.
            faceDetections = emptyList(),
            spotAe = state.spotAe.copy(active = false, status = TargetStatus.Hidden)
        )
    }

    /** Native estimate shared by display and optimistic manual entry. */
    private fun neutralSeed(state: CaptureUiState): Pair<Int, Int>? {
        val temperatureK = state.autoWhiteBalanceTemperatureK ?: return null
        val tint = state.autoWhiteBalanceTint ?: return null
        return temperatureK.coerceIn(WhiteBalanceMode.TEMP_MIN_K, WhiteBalanceMode.TEMP_MAX_K) to
            tint.coerceIn(WhiteBalanceMode.TINT_MIN, WhiteBalanceMode.TINT_MAX)
    }

    fun requestWhiteBalanceMode(state: CaptureUiState, mode: WhiteBalanceMode): CaptureUiState {
        if (mode == WhiteBalanceMode.ManualTempTint && !state.capabilities.manualWhiteBalanceSupported) return state
        if (mode != WhiteBalanceMode.Auto && mode != WhiteBalanceMode.ManualTempTint &&
            mode !in state.capabilities.supportedWhiteBalanceModes
        ) {
            return state
        }
        // Direct MANUAL entry (if ever dispatched as a mode) also seeds at the
        // current neutral state so sliders don't jump.
        if (mode == WhiteBalanceMode.ManualTempTint &&
            state.whiteBalanceMode != WhiteBalanceMode.ManualTempTint
        ) {
            neutralSeed(state)?.let { (seedTemp, seedTint) ->
                return state.copy(
                    whiteBalanceMode = mode,
                    whiteBalanceTemperatureK = seedTemp,
                    whiteBalanceTint = seedTint
                )
            }
        }
        return state.copy(whiteBalanceMode = mode)
    }

    /** Touching temperature/tint enters manual-gains mode, mirroring the MF rail.
     *
     * When entering from AUTO/preset, sliders begin at the current neutral
     * state: the dragged axis keeps its new value, the other axis seeds from
     * the last native estimate when available. The MANUAL button passes both
     * axes stale, so both seed. Already-manual drags just clamp.
     */    fun setWhiteBalanceTempTint(state: CaptureUiState, temperatureK: Int, tint: Int): CaptureUiState {
        if (!state.capabilities.manualWhiteBalanceSupported) return state
        val reqTemp = temperatureK.coerceIn(WhiteBalanceMode.TEMP_MIN_K, WhiteBalanceMode.TEMP_MAX_K)
        val reqTint = tint.coerceIn(WhiteBalanceMode.TINT_MIN, WhiteBalanceMode.TINT_MAX)
        if (state.whiteBalanceMode == WhiteBalanceMode.ManualTempTint) {
            return state.copy(
                whiteBalanceTemperatureK = reqTemp,
                whiteBalanceTint = reqTint
            )
        }
        val seed = neutralSeed(state)
        if (seed == null) {
            return state.copy(
                whiteBalanceMode = WhiteBalanceMode.ManualTempTint,
                whiteBalanceTemperatureK = reqTemp,
                whiteBalanceTint = reqTint
            )
        }
        val (seedTemp, seedTint) = seed
        val tempChanged = reqTemp != state.whiteBalanceTemperatureK
        val tintChanged = reqTint != state.whiteBalanceTint
        // Simplify: button (both stale) -> both seeded; TEMP drag -> new temp + seeded tint;
        // TINT drag -> seeded temp + new tint.
        val resolvedTemp =
            when {
                !tempChanged && !tintChanged -> seedTemp
                tempChanged && !tintChanged -> reqTemp
                !tempChanged && tintChanged -> seedTemp
                else -> reqTemp
            }
        val resolvedTint =
            when {
                !tempChanged && !tintChanged -> seedTint
                tempChanged && !tintChanged -> seedTint
                !tempChanged && tintChanged -> reqTint
                else -> reqTint
            }
        return state.copy(
            whiteBalanceMode = WhiteBalanceMode.ManualTempTint,
            whiteBalanceTemperatureK = resolvedTemp,
            whiteBalanceTint = resolvedTint
        )
    }

    /**
     * V2 lock entry: pins exactly the given temp/tint and enters manual mode
     * WITHOUT re-seeding from the HAL neutral estimate. Unlike
     * [setWhiteBalanceTempTint] (which resolves the untouched axis from AWB
     * and can visibly snap the display on first touch), this keeps whatever
     * the button currently shows, so scrubbing always continues from the
     * live value.
     */
    fun lockWhiteBalance(state: CaptureUiState, temperatureK: Int, tint: Int): CaptureUiState {
        if (!state.capabilities.manualWhiteBalanceSupported) return state
        return state.copy(
            whiteBalanceMode = WhiteBalanceMode.ManualTempTint,
            whiteBalanceTemperatureK = temperatureK.coerceIn(
                WhiteBalanceMode.TEMP_MIN_K,
                WhiteBalanceMode.TEMP_MAX_K
            ),
            whiteBalanceTint = tint.coerceIn(WhiteBalanceMode.TINT_MIN, WhiteBalanceMode.TINT_MAX)
        )
    }

    /**
     * Snaps the rails to a newly selected non-manual mode's neutral point
     * (discrete mode switch only — never continuous chasing, so AWB hunting
     * can't drag the thumbs). Falls back to the backend requested values when
     * no HAL seed is available yet. Manual mode is never touched here; manual
     * entry seeds through [setWhiteBalanceTempTint]/[requestWhiteBalanceMode].
     *
     * The native estimate is shared with the live readout and optimistic entry.
     */    fun snapWhiteBalanceDisplayToMode(
        state: CaptureUiState,
        mode: WhiteBalanceMode,
        requestedTempK: Int,
        requestedTint: Int
    ): CaptureUiState {
        if (mode == WhiteBalanceMode.ManualTempTint) return state.copy(whiteBalanceMode = mode)
        val seed = neutralSeed(state)
        if (seed == null) {
            return state.copy(
                whiteBalanceMode = mode,
                whiteBalanceTemperatureK =
                    requestedTempK.coerceIn(WhiteBalanceMode.TEMP_MIN_K, WhiteBalanceMode.TEMP_MAX_K),
                whiteBalanceTint = requestedTint.coerceIn(WhiteBalanceMode.TINT_MIN, WhiteBalanceMode.TINT_MAX)
            )
        }
        val (seedTemp, seedTint) = seed
        return state.copy(
            whiteBalanceMode = mode,
            whiteBalanceTemperatureK = seedTemp,
            whiteBalanceTint = seedTint
        )
    }

    /** Follows native values. Only readout smoothing/quantization belongs to UI. */
    fun followWhiteBalanceAutoDisplay(
        state: CaptureUiState,
        temperatureK: Int?,
        tint: Int?,
        calibrated: Boolean = false
    ): CaptureUiState {
        if (state.whiteBalanceMode == WhiteBalanceMode.ManualTempTint || temperatureK == null || tint == null) return state
        val displayTemp = (if (calibrated) (temperatureK + 25) / 50 * 50 else temperatureK)
            .coerceIn(WhiteBalanceMode.TEMP_MIN_K, WhiteBalanceMode.TEMP_MAX_K)
        val displayTint = tint.coerceIn(WhiteBalanceMode.TINT_MIN, WhiteBalanceMode.TINT_MAX)
        val nextTint = if (!calibrated || kotlin.math.abs(displayTint - state.whiteBalanceTint) >= TINT_FOLLOW_DEADBAND)
            displayTint else state.whiteBalanceTint
        if (displayTemp == state.whiteBalanceTemperatureK && nextTint == state.whiteBalanceTint) return state
        return state.copy(whiteBalanceTemperatureK = displayTemp, whiteBalanceTint = nextTint)
    }

    private const val TINT_FOLLOW_DEADBAND = 2

    fun requestExposureMode(state: CaptureUiState, mode: ExposureMode): CaptureUiState {
        // Entering a priority mode should start from the exposure the camera is
        // actually using now, not from a stale candidate remembered from an older
        // visit to that mode. NativeCameraController performs the same seeding from
        // CaptureResult; mirror it here so the rail moves immediately and does not
        // jump again when the next native snapshot arrives.
        val shutterSeed =
            if (mode == ExposureMode.ShutterPriority) {
                state.exposureApplied.shutter
                    ?.sourceCandidateId
                    ?.takeIf { state.capabilities.shutter.candidate(it) != null }
                    ?: state.exposureControl.requestedShutterId
            } else {
                state.exposureControl.requestedShutterId
            }
        val isoSeed =
            if (mode == ExposureMode.IsoPriority) {
                state.exposureApplied.iso
                    ?.sourceCandidateId
                    ?.takeIf { state.capabilities.iso.candidate(it) != null }
                    ?: state.exposureControl.requestedIsoId
            } else {
                state.exposureControl.requestedIsoId
            }
        return state.copy(
            exposureControl =
                state.exposureControl.copy(
                    mode = mode,
                    requestedShutterId = shutterSeed,
                    requestedIsoId = isoSeed
                ),
            spotAe =
                if (mode == ExposureMode.Auto) {
                    state.spotAe
                } else {
                    state.spotAe.copy(active = false, status = TargetStatus.Hidden)
                }
        )
    }

    fun requestExposureCandidate(
        state: CaptureUiState,
        parameter: ExposureParameter,
        candidateId: String
    ): CaptureUiState {
        val capability = state.capabilities.capabilityFor(parameter)
        if (capability.candidate(candidateId) == null) return state
        val control =
            when (parameter) {
                ExposureParameter.Shutter -> state.exposureControl.copy(requestedShutterId = candidateId)
                ExposureParameter.Iso -> state.exposureControl.copy(requestedIsoId = candidateId)
                ExposureParameter.Ev -> state.exposureControl.copy(requestedEvId = candidateId)
            }
        return state.copy(exposureControl = control)
    }

    /** Backend acknowledgement for a requested candidate that was actually applied. */
    fun reportAppliedExposureCandidate(
        state: CaptureUiState,
        contextGeneration: Long,
        parameter: ExposureParameter,
        candidateId: String
    ): CaptureUiState {
        if (contextGeneration != state.cameraContextGeneration) return state
        val capability = state.capabilities.capabilityFor(parameter)
        val candidate = capability.candidate(candidateId) ?: return state
        val readout = AppliedExposureReadout(candidate.displayLabel, candidate.id)
        val applied =
            when (parameter) {
                ExposureParameter.Shutter -> state.exposureApplied.copy(shutter = readout)
                ExposureParameter.Iso -> state.exposureApplied.copy(iso = readout)
                ExposureParameter.Ev -> state.exposureApplied.copy(evOrMeter = readout)
            }
        return state.copy(exposureApplied = applied)
    }

    /** Allows a real backend to report a value that does not map one-to-one to a selectable candidate. */
    fun reportAppliedExposure(
        state: CaptureUiState,
        contextGeneration: Long,
        shutter: AppliedExposureReadout,
        iso: AppliedExposureReadout,
        evOrMeter: AppliedExposureReadout
    ): CaptureUiState {
        if (contextGeneration != state.cameraContextGeneration) return state
        return state.copy(exposureApplied = ExposureAppliedState(shutter, iso, evOrMeter))
    }

    fun scrubTone(state: CaptureUiState, target: ToneParameter, delta: Int): CaptureUiState = when (target) {
        ToneParameter.Blacks -> state.copy(blacks = (state.blacks + delta).coerceIn(-100, 100))
        ToneParameter.Shadows -> state.copy(shadows = (state.shadows + delta).coerceIn(-100, 100))
        ToneParameter.Contrast -> state.copy(contrast = (state.contrast + delta).coerceIn(-100, 100))
        ToneParameter.Midtones -> state.copy(midtones = (state.midtones + delta).coerceIn(-100, 100))
        ToneParameter.Highlights -> state.copy(highlights = (state.highlights + delta).coerceIn(-100, 100))
        ToneParameter.Whites -> state.copy(whites = (state.whites + delta).coerceIn(-100, 100))
        ToneParameter.Saturation -> state.copy(saturation = (state.saturation + delta).coerceIn(-100, 100))
        ToneParameter.Vibrance -> state.copy(vibrance = (state.vibrance + delta).coerceIn(-100, 100))
    }

    fun scrubRenderExposure(state: CaptureUiState, deltaTenths: Int): CaptureUiState =
        state.copy(
            renderExposureTenths = (state.renderExposureTenths + deltaTenths).coerceIn(
                TonemapControlContract.EXPOSURE_MIN_TENTHS,
                TonemapControlContract.EXPOSURE_MAX_TENTHS
            )
        )

    /**
     * Compact EV button in full Manual drives render exposure instead of the
     * read-only meter, while the regular tonemap owns the render (Film Sim off
     * or Video) and is not LOG, where render exposure is ignored.
     */
    fun evAdjustsRenderExposure(state: CaptureUiState): Boolean =
        state.exposureControl.mode == ExposureMode.Manual &&
            (!state.filmSimEnabled || state.captureMode == CaptureMode.Video) &&
            !(state.captureMode == CaptureMode.Video && state.videoLogEnabled)

    fun selectLens(state: CaptureUiState, lensId: String): CaptureUiState =
        if (state.capabilities.lenses.any { it.id == lensId }) state.copy(selectedLensId = lensId) else state

    fun setManualFocus(state: CaptureUiState, normalized: Float): CaptureUiState {
        if (state.focus.mode != FocusMode.Mf || !state.capabilities.manualFocus.supported) return state
        return state.copy(
            focus =
                state.focus.copy(
                    mfNormalized =
                        normalized.coerceIn(
                            state.capabilities.manualFocus.minNormalized,
                            state.capabilities.manualFocus.maxNormalized
                        )
                )
        )
    }

    fun beginAutofocus(state: CaptureUiState, point: NormalizedPoint): CaptureUiState {
        if (!state.capabilities.tapAfSupported) return state
        return state.copy(
            focus =
                state.focus.copy(
                    // A tap is an AF request: MF hands back to AF at the tap point.
                    mode = if (state.focus.mode == FocusMode.Mf) FocusMode.Af else state.focus.mode,
                    target = point.clamped(),
                    status = TargetStatus.Settling,
                    selectorOpen = false
                )
        )
    }

    fun completeAutofocus(state: CaptureUiState, contextGeneration: Long, status: TargetStatus): CaptureUiState =
        if (contextGeneration == state.cameraContextGeneration) {
            state.copy(focus = state.focus.copy(status = status))
        } else {
            state
        }

    /**
     * Back to full auto: hides the tap-AF box. The native side drops the AF
     * region/trigger separately (see ClearAutofocus dispatch); this is the
     * pure UI half so previews/tests share the meaning.
     */
    fun clearAutofocus(state: CaptureUiState): CaptureUiState {
        if (state.focus.status == TargetStatus.Hidden) return state
        return state.copy(focus = state.focus.copy(status = TargetStatus.Hidden))
    }

    fun selectFocusMode(state: CaptureUiState, mode: FocusMode): CaptureUiState {
        if (mode == FocusMode.Mf && !state.capabilities.manualFocus.supported) return state
        return state.copy(
            focus =
                state.focus.copy(
                    mode = mode,
                    // Full auto shows no box; the tap box only exists while a
                    // tap-AF request is outstanding. Other modes keep the
                    // inline bottom UI visible (MF rail can be adjusted
                    // immediately; dismiss via scrim tap).
                    status = if (mode == FocusMode.Af) TargetStatus.Hidden else TargetStatus.Settled
                )
        )
    }

    fun beginSpotAe(state: CaptureUiState, point: NormalizedPoint): CaptureUiState = state.copy(
        spotAe = SpotAeUiState(active = true, target = point.clamped(), status = TargetStatus.Settling)
    )

    fun moveSpotAe(state: CaptureUiState, point: NormalizedPoint): CaptureUiState = state.copy(
        spotAe = state.spotAe.copy(active = true, target = point.clamped(), status = TargetStatus.Settling)
    )

    fun cancelSpotAe(state: CaptureUiState): CaptureUiState = state.copy(
        spotAe = state.spotAe.copy(active = false, status = TargetStatus.Hidden)
    )

    fun completeSpotAe(state: CaptureUiState, contextGeneration: Long, status: TargetStatus): CaptureUiState =
        if (contextGeneration == state.cameraContextGeneration) {
            state.copy(spotAe = state.spotAe.copy(status = status))
        } else {
            state
        }

    fun enqueueCapture(state: CaptureUiState, capture: PendingCapture): CaptureUiState =
        state.copy(pendingCaptures = state.pendingCaptures + capture, captureFlash = true)

    /**
     * Cancels any active self-timer countdown (mode/lens switch, capability
     * regen). Bumps the run id so in-flight ticker jobs go stale even if
     * their cancel races a tick.
     */
    fun cancelSelfTimerRun(state: CaptureUiState): CaptureUiState =
        if (state.selfTimerRemainingMs == null) {
            state
        } else {
            state.copy(
                selfTimerRemainingMs = null,
                selfTimerRunId = state.selfTimerRunId + 1L
            )
        }

    /** True while a self-timer countdown is on screen (shutter/record gated). */
    fun isSelfTimerCounting(state: CaptureUiState): Boolean = state.selfTimerRemainingMs != null

    fun updateCapturePhase(state: CaptureUiState, captureId: Long, phase: CaptureSavePhase): CaptureUiState {
        if (state.pendingCaptures.none { it.id == captureId }) return state
        return state.copy(
            pendingCaptures =
                state.pendingCaptures.map { capture ->
                    if (capture.id == captureId) capture.copy(phase = phase) else capture
                }
        )
    }

    fun setCaptureFlash(state: CaptureUiState, visible: Boolean): CaptureUiState = state.copy(captureFlash = visible)

    fun completeCapture(state: CaptureUiState, captureId: Long): CaptureUiState {
        if (state.pendingCaptures.none { it.id == captureId }) return state
        return state.copy(
            pendingCaptures = state.pendingCaptures.filterNot { it.id == captureId },
            thumbnailRevision = state.thumbnailRevision + 1
        )
    }

    /**
     * V2 compact-button lock/auto mapping. Locks reuse the global [ExposureMode]
     * (no per-parameter flags): locking an auto axis moves into the matching
     * priority/manual mode, unlocking returns toward Auto. Returns null when no
     * mode change is needed (already locked/unlocked or unsupported).
     */
    fun lockModeFor(parameter: ExposureParameter, mode: ExposureMode): ExposureMode? = when (parameter) {
        ExposureParameter.Shutter -> when (mode) {
            ExposureMode.Auto -> ExposureMode.ShutterPriority
            ExposureMode.IsoPriority -> ExposureMode.Manual
            else -> null
        }
        ExposureParameter.Iso -> when (mode) {
            ExposureMode.Auto -> ExposureMode.IsoPriority
            ExposureMode.ShutterPriority -> ExposureMode.Manual
            else -> null
        }
        ExposureParameter.Ev -> null
    }

    fun unlockModeFor(parameter: ExposureParameter, mode: ExposureMode): ExposureMode? = when (parameter) {
        ExposureParameter.Shutter -> when (mode) {
            ExposureMode.Manual -> ExposureMode.IsoPriority
            ExposureMode.ShutterPriority -> ExposureMode.Auto
            else -> null
        }
        ExposureParameter.Iso -> when (mode) {
            ExposureMode.Manual -> ExposureMode.ShutterPriority
            ExposureMode.IsoPriority -> ExposureMode.Auto
            else -> null
        }
        ExposureParameter.Ev -> null
    }

    /** True when the compact button for [parameter] represents a locked (manual) axis. */
    fun isParameterLocked(state: CaptureUiState, parameter: ExposureParameter): Boolean = when (parameter) {
        ExposureParameter.Shutter -> state.exposureControl.mode == ExposureMode.Manual ||
            state.exposureControl.mode == ExposureMode.ShutterPriority
        ExposureParameter.Iso -> state.exposureControl.mode == ExposureMode.Manual ||
            state.exposureControl.mode == ExposureMode.IsoPriority
        ExposureParameter.Ev -> state.exposureControl.mode != ExposureMode.Manual
    }

    fun isWhiteBalanceLocked(state: CaptureUiState): Boolean =
        state.whiteBalanceMode == WhiteBalanceMode.ManualTempTint
}
