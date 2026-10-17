package com.rawr.camera.ui

import androidx.compose.runtime.Composable
import androidx.compose.ui.tooling.preview.Preview
import com.rawr.camera.architecture.CaptureDispatch
import com.rawr.camera.fixtures.CaptureFixtures
import com.rawr.camera.model.*

private val caps = CaptureFixtures.baseline()
private val noop: CaptureDispatch = {}

@Composable private fun PreviewState(state: CaptureUiState) = CaptureTheme { CaptureScreen(state, noop) }

private fun base() = CaptureUiState(caps, selectedLensId = "main")

@Preview(name = "01 Auto exposure", widthDp = 360, heightDp = 800)
@Composable
fun PreviewAuto() = PreviewState(base())

@Preview(name = "02 Manual exposure", widthDp = 360, heightDp = 800)
@Composable
fun PreviewManual() = PreviewState(base().withExposureMode(ExposureMode.Manual))

@Preview(name = "03 AF settling", widthDp = 360, heightDp = 800)
@Composable
fun PreviewAfSettling() = PreviewState(base().copy(focus = base().focus.copy(status = TargetStatus.Settling)))

@Preview(name = "04 AF settled", widthDp = 360, heightDp = 800)
@Composable
fun PreviewAfSettled() = PreviewState(base().copy(focus = base().focus.copy(status = TargetStatus.Settled)))

@Preview(name = "05 AF locked", widthDp = 360, heightDp = 800)
@Composable
fun PreviewAfLocked() =
    PreviewState(base().copy(focus = base().focus.copy(mode = FocusMode.AfLock, status = TargetStatus.Settled)))

@Preview(name = "06 AF failed", widthDp = 360, heightDp = 800)
@Composable
fun PreviewAfFailed() = PreviewState(base().copy(focus = base().focus.copy(status = TargetStatus.Failed)))

@Preview(name = "07 Spot AE settling", widthDp = 360, heightDp = 800)
@Composable
fun PreviewSpotSettling() =
    PreviewState(base().copy(spotAe = SpotAeUiState(true, NormalizedPoint(.68f, .45f), TargetStatus.Settling)))

@Preview(name = "08 Spot AE settled", widthDp = 360, heightDp = 800)
@Composable
fun PreviewSpotSettled() =
    PreviewState(base().copy(spotAe = SpotAeUiState(true, NormalizedPoint(.68f, .45f), TargetStatus.Settled)))

@Preview(name = "09 Spot AE constrained", widthDp = 360, heightDp = 800)
@Composable
fun PreviewSpotFailed() =
    PreviewState(base().copy(spotAe = SpotAeUiState(true, NormalizedPoint(.68f, .45f), TargetStatus.Failed)))

@Preview(name = "10 MF active", widthDp = 360, heightDp = 800)
@Composable
fun PreviewMf() = PreviewState(
    base().copy(focus = base().focus.copy(mode = FocusMode.Mf, selectorOpen = true, status = TargetStatus.Settled))
)

@Preview(name = "11 One scope", widthDp = 360, heightDp = 800)
@Composable
fun PreviewOneScope() = PreviewState(base().copy(activeScopes = listOf(ScopeType.Waveform)))

@Preview(name = "12 Two scopes", widthDp = 360, heightDp = 800)
@Composable
fun PreviewTwoScopes() = PreviewState(base().copy(activeScopes = listOf(ScopeType.Waveform, ScopeType.Vectorscope)))

@Preview(name = "13 Two scopes", widthDp = 360, heightDp = 800)
@Composable
fun PreviewThreeScopes() = PreviewState(base().copy(activeScopes = ScopeType.entries))

@Preview(name = "15 Expanded waveform", widthDp = 360, heightDp = 800)
@Composable
fun PreviewWaveExpanded() =
    PreviewState(base().copy(activeScopes = listOf(ScopeType.Waveform), expandedScope = ScopeType.Waveform))

@Preview(name = "16 Expanded vectorscope", widthDp = 360, heightDp = 800)
@Composable
fun PreviewVectorExpanded() =
    PreviewState(base().copy(activeScopes = listOf(ScopeType.Vectorscope), expandedScope = ScopeType.Vectorscope))

@Preview(name = "17 Compact processing glow", widthDp = 360, heightDp = 800)
@Composable
fun PreviewProcessing() = PreviewState(
    base().copy(
        captureLayout = CaptureControlLayout.Compact,
        pendingCaptures =
            listOf(
                PendingCapture(1, jpegEnabled = true, phase = CaptureSavePhase.JpegProcessing),
                PendingCapture(2, jpegEnabled = true, phase = CaptureSavePhase.DngProcessing)
            )
    )
)

@Preview(name = "17b Save failed border", widthDp = 360, heightDp = 800)
@Composable
fun PreviewProcessingFailed() = PreviewState(
    base().copy(
        captureLayout = CaptureControlLayout.Compact,
        pendingCaptures =
            listOf(
                PendingCapture(1, jpegEnabled = true, phase = CaptureSavePhase.JpegFailed)
            )
    )
)

@Preview(name = "17c Save success glow", widthDp = 360, heightDp = 800)
@Composable
fun PreviewSaveSuccess() = PreviewState(
    base().copy(
        captureLayout = CaptureControlLayout.Compact,
        pendingCaptures =
            listOf(
                PendingCapture(1, jpegEnabled = true, phase = CaptureSavePhase.JpegSaved)
            )
    )
)

@Preview(name = "18 Portrait", widthDp = 360, heightDp = 800)
@Composable
fun PreviewPortrait() = PreviewState(base().copy(orientation = Orientation.Portrait))

@Preview(name = "19 Physical landscape posture", widthDp = 360, heightDp = 800)
@Composable
fun PreviewLandscape() = PreviewState(
    base().copy(
        orientation = Orientation.Landscape,
        activeScopes = listOf(ScopeType.Waveform, ScopeType.Vectorscope)
    )
)

// Review-gate parity exports. Export these four directly from Compose Preview.
@Preview(name = "PARITY Portrait Auto 360x800", widthDp = 360, heightDp = 800)
@Composable
fun ParityPortraitAuto() = PreviewState(base().copy(orientation = Orientation.Portrait))

@Preview(name = "PARITY Portrait Manual 360x800", widthDp = 360, heightDp = 800)
@Composable
fun ParityPortraitManual() =
    PreviewState(base().copy(orientation = Orientation.Portrait).withExposureMode(ExposureMode.Manual))

@Preview(name = "PARITY Physical landscape Auto", widthDp = 360, heightDp = 800)
@Composable
fun ParityLandscapeAuto() = PreviewState(base().copy(orientation = Orientation.Landscape))

@Preview(name = "PARITY Physical landscape 2 scopes", widthDp = 360, heightDp = 800)
@Composable
fun ParityLandscapeThreeScopes() =
    PreviewState(base().copy(orientation = Orientation.Landscape, activeScopes = ScopeType.entries))

@Preview(name = "STYLE Frosted Manual", widthDp = 360, heightDp = 800)
@Composable
fun PreviewFrostedStyle() =
    PreviewState(base().copy(controlSurfaceStyle = ControlSurfaceStyle.Frosted).withExposureMode(ExposureMode.Manual))

@Preview(name = "STYLE Basic Manual", widthDp = 360, heightDp = 800)
@Composable
fun PreviewBasicStyle() =
    PreviewState(base().copy(controlSurfaceStyle = ControlSurfaceStyle.Basic).withExposureMode(ExposureMode.Manual))

private fun CaptureUiState.withExposureMode(mode: ExposureMode) =
    copy(exposureControl = exposureControl.copy(mode = mode))

private fun filmQuickSample() = FilmSimQuickState(
    activePresetName = "Portra 400 Natural",
    selectedPresetId = "portra400",
    modified = false,
    presets = listOf(
        FilmSimQuickOption("portra400", "Portra 400 Natural"),
        FilmSimQuickOption("gold200", "Gold 200 Warm"),
        FilmSimQuickOption("velvia", "Velvia Vivid")
    ),
    params = mapOf(
        "FilmExposureEv" to FilmSimQuickParam("FilmExposureEv", "FILM EV", 0.3f, 0f, -5f, 5f, 0.1f, 1, "+0.3", false),
        "FilmPushPullStops" to FilmSimQuickParam("FilmPushPullStops", "PUSHPL", 0f, 0f, -2f, 2f, 0.1f, 1, "+0.0", true),
        "PrintExposureEv" to FilmSimQuickParam("PrintExposureEv", "PRINT EV", 0f, 0f, -5f, 5f, 0.1f, 1, "+0.0", true),
        "PrintGamma" to FilmSimQuickParam("PrintGamma", "PRINT CON", 1f, 1f, 0.2f, 2f, 0.01f, 2, "1.00", true),
        "GrainAmount" to FilmSimQuickParam("GrainAmount", "GRAIN", 0.7f, 1f, 0f, 3f, 0.05f, 2, "0.70", false)
    ),
    grainEnabled = false
)

@Preview(name = "20 Film strip collapsed", widthDp = 360, heightDp = 800)
@Composable
fun PreviewFilmStrip() = CaptureTheme {
    CaptureScreen(base().copy(filmSimEnabled = true), noop, filmQuick = filmQuickSample())
}

@Preview(name = "21 Film strip custom modified", widthDp = 360, heightDp = 800)
@Composable
fun PreviewFilmCustom() = CaptureTheme {
    CaptureScreen(
        base().copy(filmSimEnabled = true),
        noop,
        filmQuick = filmQuickSample().copy(
            activePresetName = "Custom",
            selectedPresetId = null,
            modified = false
        )
    )
}

private fun renderProfilesSample() = RenderProfileQuickState(
    options = listOf(
        RenderProfileQuickOption(com.rawr.camera.model.RenderProfileSelection.BuiltIn(com.rawr.camera.settings.model.ColorRenderProfile.RawrBase), "RAWR NTRL"),
        RenderProfileQuickOption(com.rawr.camera.model.RenderProfileSelection.Imported("portra"), "Portra LUT"),
        RenderProfileQuickOption(com.rawr.camera.model.RenderProfileSelection.Imported("cine"), "Cine LUT")
    ),
    selectedId = com.rawr.camera.model.RenderProfileSelection.Imported("portra")
)

@Preview(name = "22 Tonemap strip", widthDp = 360, heightDp = 800)
@Composable
fun PreviewTonemapStrip() = CaptureTheme {
    CaptureScreen(base(), noop, renderProfiles = renderProfilesSample())
}
