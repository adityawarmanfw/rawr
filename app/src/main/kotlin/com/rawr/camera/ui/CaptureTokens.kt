package com.rawr.camera.ui

import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp

internal object CaptureColors {
    val Background = Color(0xFF020202)
    val Surface = Color(0xFF080808)
    val Panel = Color(0xFF101010)
    val Accent = Color(0xFFE06A61)
    val AccentSoft = Color(0xFFF2C7AA)
    val Danger = Color(0xFFFF6961)
    val Success = Color(0xFF65D17A)

    val GlassTop = Color(0xFF171A1C).copy(alpha = .42f)
    val GlassMiddle = Color(0xFF090B0D).copy(alpha = .76f)
    val GlassBottom = Color(0xFF020304).copy(alpha = .88f)
    val GlassBorderActive = Accent.copy(alpha = .30f)
    val ViewfinderPanel = Color(0xFF07090B).copy(alpha = .66f)

    val Line = Color.White.copy(alpha = .085f)
    val StrongLine = Color.White.copy(alpha = .14f)
    val Muted = Color.White.copy(alpha = .46f)
}

internal object CaptureDimens {
    val TopBarHeight = 48.dp
    val MonitorHeight = 22.dp
    val VideoControlsClassicHeight = 174.dp
    // Lens 30 + compact shutter row 78 + video record row 62.
    val CompactVideoControlsMinimumHeight = 170.dp
    val VideoRecordRowHeight = 62.dp
    val VideoMonitorBottomInset = 6.dp
    val QuickRowHeight = 38.dp
    val ExposureControlHeight = QuickRowHeight
    val ViewfinderHorizontalPadding = 12.dp
    val ViewfinderBottomPadding = 10.dp
    val ControlGap = 6.dp
    val ShutterOuter = 70.dp
    val ShutterInner = 60.dp
    /** White STOP square inside the record ring while recording. */
    val VideoStopInner = 30.dp
    val Thumbnail = 48.dp
    val ScopeCompactWidth = 112.dp
    val ScopeCompactHeight = 84.dp

    // Shared compact top-bar palette geometry. COLOR RENDER and MONITORING use
    // the same shell so tuning one does not silently diverge from the other.
    val TopBarPalettePortraitWidth = 300.dp
    val TopBarPaletteLandscapeWidth = 250.dp
    val TopBarPaletteBodyHeight = 128.dp
    val TopBarPaletteProfileViewportHeight = 108.dp
    val TopBarPaletteTabHeight = 20.dp
    val TopBarPaletteMajorChoiceHeight = 32.dp
    val TopBarPaletteMinorChoiceHeight = 27.dp
    val TopBarPaletteSplitGap = 8.dp
    val TopBarPaletteInnerGap = 4.dp

    // Placement belongs to the viewfinder composition because it is relative to
    // scopes and exposure controls, not to the palette content itself.
    val TopBarPalettePortraitBottomPadding = 88.dp
    val TopBarPaletteLandscapeStartOffset = (-21).dp
    val TopBarPaletteLandscapeCrossOffset = 12.dp

    val ExposureMinorTickHeight = 6.dp
    val ExposureMajorTickHeight = 10.dp
    val ExposureHorizontalInset = 12.dp
    val ExposureCornerRadius = 11.dp
    val ExposureCenterGap = 62.dp
    val ExposureRailCenterY = 19.dp
    val ExposureIndexTop = 2.dp
    val ExposureIndexTopEnd = 6.dp
    val ExposureIndexBottomStart = 32.dp
    val ExposureIndexBottom = 36.dp
    val ExposureDetentSpacing = 24.dp
    val GlassControlRadius = 11.dp

    // V2 compact button strip between ExposureMonitor and the shutter row.
    // Two-line stacked height: value above, icon + label below.
    val CompactParamRowHeight = 30.dp
    val CompactParamTwoLineHeight = 46.dp
    val CompactParamRowEdgeInset = 8.dp
    val CompactParamColumnWeights = listOf(1f, 1f, 1f, 1.15f, 1f)
    val CompactParamRadius = 12.dp
    val CompactPillHeight = 48.dp
    val CompactPillMinWidth = 56.dp
}

internal val CaptureMono = FontFamily.Monospace
