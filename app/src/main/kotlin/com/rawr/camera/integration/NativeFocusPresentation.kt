package com.rawr.camera.integration

import com.rawr.camera.architecture.CaptureTransitions
import com.rawr.camera.model.*

/** UI acknowledgment/display projection. Native owns AF regions and their lifetime. */
internal object NativeFocusPresentation {
    /** Seeds only the pending rail display; the mode command carries no computed lens position. */
    fun requestMode(current: CaptureUiState, mode: FocusMode): CaptureUiState {
        if (mode == FocusMode.Mf && !current.capabilities.manualFocus.supported) return current
        if (mode == FocusMode.AfLock && !current.capabilities.tapAfSupported) return current
        val next = CaptureTransitions.selectFocusMode(current, mode)
        if (mode != FocusMode.Mf || current.focus.mode == FocusMode.Mf) return next
        val diopters = current.focus.appliedFocusDiopters ?: return next
        val minimum = current.capabilities.manualFocus.minimumFocusDistance
        if (!diopters.isFinite() || !minimum.isFinite()) return next
        val seed = mfNormalizedFromDiopters(diopters, minimum) ?: return next
        return CaptureTransitions.setManualFocus(next, seed)
    }

    fun requestManualFocus(current: CaptureUiState, normalized: Float): CaptureUiState {
        if (!normalized.isFinite() || !current.capabilities.manualFocus.supported) return current
        return CaptureTransitions.setManualFocus(CaptureTransitions.selectFocusMode(current, FocusMode.Mf), normalized)
    }

    fun project(
        current: FocusUiState,
        snapshot: NativeCameraUiSnapshot,
        latestRequestId: Long,
        distanceReadoutTrustworthy: Boolean
    ): FocusUiState {
        val telemetry = current.copy(
            appliedFocusDiopters = snapshot.appliedFocusDistance,
            backendNativeReadout = if (distanceReadoutTrustworthy)
                snapshot.appliedFocusDistance?.let(ExposureFormat::formatFocusDistance) else null
        )
        // A snapshot predating the gesture cannot roll back its box, mode or
        // rail. Rejected commands are acknowledged too and project normally.
        if (snapshot.focusRequestId < latestRequestId) return telemetry
        return telemetry.copy(
            mode = when (snapshot.focusMode) {
                1 -> FocusMode.AfLock
                2 -> FocusMode.Mf
                else -> FocusMode.Af
            },
            status = if (!snapshot.tapAfActive && snapshot.focusMode == 0) TargetStatus.Hidden else when (snapshot.afState) {
                1, 3 -> TargetStatus.Settling
                2, 4 -> TargetStatus.Settled
                5, 6 -> TargetStatus.Failed
                else -> TargetStatus.Settling
            },
            mfNormalized = snapshot.requestedManualFocusNormalized.coerceIn(0f, 1f)
        )
    }
}
