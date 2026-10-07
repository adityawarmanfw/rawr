package com.rawr.camera.model

enum class ExposureMode(val shortLabel: String) { Auto("A"), Manual("M"), ShutterPriority("S"), IsoPriority("I") }

enum class ExposureParameter { Shutter, Iso, Ev }

/** Order the Pro layout's MODE tile steps through: Auto, then the priority modes, then full Manual. */
private val exposureModeCycle = listOf(
    ExposureMode.Auto,
    ExposureMode.ShutterPriority,
    ExposureMode.IsoPriority,
    ExposureMode.Manual
)

/** Next mode for a tap on the MODE tile, skipping modes this camera cannot do. Returns [current] if nothing else fits. */
fun nextExposureMode(current: ExposureMode, supported: Set<ExposureMode>): ExposureMode {
    val start = exposureModeCycle.indexOf(current)
    for (step in 1..exposureModeCycle.size) {
        val candidate = exposureModeCycle[(start + step) % exposureModeCycle.size]
        if (candidate == ExposureMode.Auto || candidate in supported) return candidate
    }
    return current
}

/** Words for the MODE tile. */
val ExposureMode.chipLabel: String
    get() = when (this) {
        ExposureMode.Auto -> "AUTO"
        ExposureMode.ShutterPriority -> "SHUTTER"
        ExposureMode.IsoPriority -> "ISO"
        ExposureMode.Manual -> "MANUAL"
    }

enum class ToneParameter {
    Blacks,
    Shadows,
    Contrast,
    Midtones,
    Highlights,
    Whites,
    Saturation,
    Vibrance
}

enum class Orientation { Portrait, Landscape }

enum class CaptureMode { Photo, Video }

enum class GridMode { Off, Thirds, FourByFour, Cross }

enum class OverlayMode { Peaking, RawHighlights, TonemapShadows, FalseColor }

enum class ScopeType { Waveform, Vectorscope }

enum class WaveformMode { Luma, RgbOverlay }

enum class TargetStatus { Hidden, Settling, Settled, Failed }

enum class FocusMode { Af, AfLock, Mf }

data class NormalizedPoint(val x: Float, val y: Float) {
    fun clamped(margin: Float = .05f) = copy(
        x = x.coerceIn(margin, 1f - margin),
        y = y.coerceIn(margin, 1f - margin)
    )
}

/** Face detection box in display-normalized viewfinder coordinates. */
data class FaceDetection(
    val x: Float,
    val y: Float,
    val w: Float,
    val h: Float,
    /** 1..100, higher is more confident. Strongest first. */
    val score: Int
)

data class FocusUiState(
    val mode: FocusMode = FocusMode.Af,
    val target: NormalizedPoint = NormalizedPoint(.5f, .46f),
    val status: TargetStatus = TargetStatus.Settled,
    val selectorOpen: Boolean = false,
    val mfNormalized: Float = .57f,
    /** Live lens position from CaptureResult in diopters; null before first result. */
    val appliedFocusDiopters: Float? = null,
    val backendNativeReadout: String? = null
)

data class SpotAeUiState(
    val active: Boolean = false,
    val target: NormalizedPoint = NormalizedPoint(.68f, .45f),
    val status: TargetStatus = TargetStatus.Hidden
)

data class ExposureControlState(
    val mode: ExposureMode,
    val requestedShutterId: String,
    val requestedIsoId: String,
    val requestedEvId: String
) {
    companion object {
        fun initial(capabilities: CameraCapabilities) = ExposureControlState(
            mode = ExposureMode.Auto,
            requestedShutterId = capabilities.shutter.initialCandidateId,
            requestedIsoId = capabilities.iso.initialCandidateId,
            requestedEvId = capabilities.ev.initialCandidateId
        )
    }
}

/**
 * A value reported/applied by the camera/application pipeline for the compact monitor.
 * It is intentionally separate from the requested control candidate.
 *
 * [sourceCandidateId] is optional because a backend may report an applied value that does
 * not exactly correspond to one selectable candidate (clamping, AE, camera switch, etc.).
 */
data class AppliedExposureReadout(val displayLabel: String, val sourceCandidateId: String? = null)

data class ExposureAppliedState(
    val shutter: AppliedExposureReadout?,
    val iso: AppliedExposureReadout?,
    val evOrMeter: AppliedExposureReadout?
) {
    companion object {
        fun initial(capabilities: CameraCapabilities) = ExposureAppliedState(
            shutter = capabilities.shutter.initialCandidate.toAppliedReadout(),
            iso = capabilities.iso.initialCandidate.toAppliedReadout(),
            evOrMeter = capabilities.ev.initialCandidate.toAppliedReadout()
        )

        /** No applied observation is valid for the current camera context yet. */
        fun pending() = ExposureAppliedState(shutter = null, iso = null, evOrMeter = null)
    }
}

private fun ExposureCandidate.toAppliedReadout() = AppliedExposureReadout(
    displayLabel = displayLabel,
    sourceCandidateId = id
)

enum class CaptureSavePhase {
    DngProcessing,
    DngSaved,
    DngFailed,
    JpegProcessing,
    JpegSaved,
    JpegFailed
}

data class PendingCapture(
    val id: Long,
    val jpegEnabled: Boolean,
    val phase: CaptureSavePhase = CaptureSavePhase.DngProcessing,
    val terminal: Boolean = false
)

data class CaptureUiState(
    val capabilities: CameraCapabilities,
    /** Monotonic identity for the active physical-camera/capability context. */
    val cameraContextGeneration: Long = 0L,
    val orientation: Orientation = Orientation.Portrait,
    val captureMode: CaptureMode = CaptureMode.Photo,
    val videoResolution: com.rawr.camera.video.VideoResolutionMode =
        com.rawr.camera.video.VideoResolutionMode.HD1080,
    /** Recording frame rate; only 24 and 30 are supported by the native recorder. */
    val videoFps: Int = 30,
    val videoLogEnabled: Boolean = false,
    val jpegEnabled: Boolean = true,
    val dngEnabled: Boolean = true,
    val grid: GridMode = GridMode.Off,
    val exposureControl: ExposureControlState = ExposureControlState.initial(capabilities),
    val exposureApplied: ExposureAppliedState = ExposureAppliedState.initial(capabilities),
    /** Camera2 POST_RAW_SENSITIVITY_BOOST result (100 == 1x); null before first result. */
    val sensitivityBoost: Int? = null,
    /** Sensor-reported frame rate; null when the HAL hasn't echoed it yet. */
    val rawFps: Double? = null,
    /** Measured viewfinder rate (timestamp-delta EMA); null until settled. */
    val viewfinderFps: Double? = null,
    val blacks: Int = 0,
    val shadows: Int = 0,
    val contrast: Int = 0,
    val midtones: Int = 0,
    val highlights: Int = 0,
    val whites: Int = 0,
    val saturation: Int = 0,
    val vibrance: Int = 0,
    /** Active profile's render exposure in 0.1 EV steps (Tone "Render Exposure"). */
    val renderExposureTenths: Int = 0,
    val selectedLensId: String = capabilities.lenses.first().id,
    val whiteBalanceMode: WhiteBalanceMode = WhiteBalanceMode.Auto,
    val whiteBalanceTemperatureK: Int = WhiteBalanceMode.TEMP_DEFAULT_K,
    val whiteBalanceTint: Int = 0,
    // Native resolves calibrated CCT/tint or the gains fallback. UI uses these
    // values only for display and optimistic entry; no sensor math lives here.
    val autoWhiteBalanceTemperatureK: Int? = null,
    val autoWhiteBalanceTint: Int? = null,
    val whiteBalancePanelOpen: Boolean = false,
    /** Auto overlays armed by the user; each renders only while its interaction trigger fires. */
    val armedOverlays: Set<OverlayMode> = emptySet(),
    /** Manual exclusive FalseColor, independent of the auto trigger gates. */
    val falseColorManual: Boolean = false,
    val activeScopes: List<ScopeType> = emptyList(),
    val expandedScope: ScopeType? = null,
    val waveformMode: WaveformMode = WaveformMode.Luma,
    val monitorPanelOpen: Boolean = false,
    /** Film simulation owns the render: the tonemap strip and tone value caches yield to it. */
    val filmSimEnabled: Boolean = false,
    val focus: FocusUiState = FocusUiState(),
    /** Live face detections driving face-priority AF (strongest first). */
    val faceDetections: List<FaceDetection> = emptyList(),
    val spotAe: SpotAeUiState = SpotAeUiState(),
    val pendingCaptures: List<PendingCapture> = emptyList(),
    val thumbnailRevision: Int = 0,
    val captureFlash: Boolean = false,
    val controlSurfaceStyle: ControlSurfaceStyle = ControlSurfaceStyle.Basic,
    /** V1 rails vs V2 compact buttons. Presentation-only; persisted via settings. */
    val captureLayout: CaptureControlLayout = CaptureControlLayout.Compact,
    /** Self-timer delay; Off means immediate shutter. Persisted via settings. */
    val selfTimer: com.rawr.camera.settings.model.SelfTimer =
        com.rawr.camera.settings.model.SelfTimer.Off,
    /** Remaining countdown in ms while a self-timer run is active; null when idle. */
    val selfTimerRemainingMs: Long? = null,
    /** Monotonic run id; ticks from a stale run are ignored. */
    val selfTimerRunId: Long = 0L,
    /** Mirrors settings experimentalMultiframeEnabled for the V2 shutter-row toggle. */
    val experimentalMultiframeEnabled: Boolean = false
)
