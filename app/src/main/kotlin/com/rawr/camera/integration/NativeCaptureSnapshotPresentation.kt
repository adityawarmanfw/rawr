package com.rawr.camera.integration

import com.rawr.camera.architecture.CaptureTransitions
import com.rawr.camera.model.*

/**
 * Owns capability projection/cache and composes snapshot presentation. Has no
 * coordinator, coroutine scope, timers or native commands. Preparation happens
 * once; the resulting state transform can be retried by the controller's CAS.
 */
internal class NativeCaptureSnapshotPresentation {
    private var context: CapabilityContext? = null
    @Volatile var exposureProjection: NativeExposureProjection? = null
        private set

    @Synchronized
    fun invalidate() {
        context = null
        exposureProjection = null
    }

    @Synchronized
    fun exposureProjectionFor(current: CaptureUiState): NativeExposureProjection? {
        val videoFps = current.videoFps.takeIf { current.captureMode == CaptureMode.Video }
        return exposureProjection.takeIf { context?.videoFps == videoFps && context?.lensId == current.selectedLensId }
    }

    @Synchronized
    fun prepare(current: CaptureUiState, snapshot: NativeCameraUiSnapshot): PreparedCaptureSnapshot? {
        val videoFps = current.videoFps.takeIf { current.captureMode == CaptureMode.Video }
        if (!matchesCaptureMode(current, snapshot)) return null
        val nextContext = CapabilityContext.from(snapshot, videoFps)
        val contextChanged = context != nextContext
        val videoFpsChanged = context?.videoFps != videoFps
        if (contextChanged) {
            exposureProjection = NativeCapabilityProjection.project(snapshot, videoFps)
            context = nextContext
        }
        return PreparedCaptureSnapshot(snapshot, exposureProjection ?: return null, contextChanged || current.capabilities !== exposureProjection?.capabilities, videoFpsChanged)
    }

    // Cache only capability facts, so per-frame telemetry never rebuilds grids.
    private data class CapabilityContext(
        val generation: Long,
        val lensId: String,
        val isoMin: Int,
        val isoMax: Int,
        val shutterMin: Long,
        val shutterMax: Long,
        val evMin: Int,
        val evMax: Int,
        val evStep: Double,
        val manualExposure: Boolean,
        val shutterPriority: Boolean,
        val isoPriority: Boolean,
        val tapAf: Boolean,
        val manualFocus: Boolean,
        val focusReadout: Boolean,
        val focusDistance: Float,
        val hyperfocalDistance: Float,
        val manualWb: Boolean,
        val awbModes: List<Int>,
        val videoFps: Int?,
        val angles: List<NativeShutterAngleChoice>,
        // Lens settings can change the list without restarting the camera.
        val lenses: List<Pair<String, String>>
    ) {
        companion object {
            fun from(s: NativeCameraUiSnapshot, fps: Int?) = CapabilityContext(
                s.generation, s.lensId, s.sensitivityMin, s.sensitivityMax,
                s.exposureTimeMinNs, s.exposureTimeMaxNs, s.evMinSteps, s.evMaxSteps, s.evStep,
                s.manualExposureSupported, s.shutterPrioritySupported, s.isoPrioritySupported,
                s.tapAfSupported, s.manualFocusSupported, s.focusDistanceReadoutTrustworthy,
                s.minimumFocusDistance, s.hyperfocalDistance, s.manualWhiteBalanceSupported, s.supportedAwbModes,
                fps, if (fps != null) s.shutterAngleChoices else emptyList(), s.profileLenses
            )
        }
    }
}

internal class PreparedCaptureSnapshot(
    private val snapshot: NativeCameraUiSnapshot,
    private val projection: NativeExposureProjection,
    val contextChanged: Boolean,
    private val videoFpsChanged: Boolean
) {
    fun project(current: CaptureUiState, latestFocusRequestId: Long, latestWhiteBalanceRequestId: Long): CaptureUiState {
        // A mode/FPS gesture may arrive between preparation and CAS retry.
        if (!matchesCaptureMode(current, snapshot)) return current
        val reconciled = if (contextChanged) {
            CaptureTransitions.cancelSelfTimerRun(CaptureTransitions.applyCameraCapabilities(current, projection.capabilities))
                .copy(selectedLensId = snapshot.lensId)
        } else current
        val exposure = NativeExposurePresentation.project(reconciled, snapshot, projection, videoFpsChanged)
        val whiteBalance = NativeWhiteBalancePresentation.project(exposure, snapshot, latestWhiteBalanceRequestId)
        return whiteBalance.copy(
            focus = NativeFocusPresentation.project(
                whiteBalance.focus, snapshot, latestFocusRequestId,
                projection.capabilities.manualFocus.distanceReadoutTrustworthy
            )
        )
    }
}

// A snapshot for another lens is stale while a lens switch is pending. Native
// picks the lens itself at startup (no selection yet) and when the selected
// lens left the profile (renamed/removed/disabled); then its lens is adopted.
private fun matchesCaptureMode(current: CaptureUiState, snapshot: NativeCameraUiSnapshot): Boolean =
    (snapshot.lensId == current.selectedLensId || current.selectedLensId.isEmpty() ||
        (snapshot.profileLenses.isNotEmpty() && snapshot.profileLenses.none { it.first == current.selectedLensId })) &&
        snapshot.videoMode == (current.captureMode == CaptureMode.Video) &&
        (current.captureMode != CaptureMode.Video || snapshot.videoPreviewFps == current.videoFps)
