package com.rawr.camera.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.PlatformTextStyle
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.rawr.camera.model.CaptureMonitorState
import com.rawr.camera.model.ExposureFormat
import com.rawr.camera.model.ExposureMode

@Composable
internal fun ExposureMonitor(state: CaptureMonitorState) {
    val meterLabel = if (state.exposureMode == ExposureMode.Manual) "METER" else "EV"
    // Same WB source as the compact strip and drawer rails
    // (live HAL estimate in auto/preset, requested sliders in manual),
    // so the three can never drift apart.
    val wbTint = state.whiteBalanceTint
    val wbValue = "${state.whiteBalanceTemperatureK}K ${if (wbTint >= 0) "+$wbTint" else "$wbTint"}"
    // Focus is always meters in the monitor (calibration flag only gates the
    // MF rail fallback); null before the first CaptureResult.
    val focusValue = state.focusDiopters?.let(ExposureFormat::formatFocusDistance) ?: "—"
    val fpsValue =
        when {
            state.rawFps != null && state.viewfinderFps != null ->
                "%.2f/%.2f".format(state.rawFps, state.viewfinderFps)
            state.rawFps != null -> "%.2f".format(state.rawFps)
            state.viewfinderFps != null -> "%.2f".format(state.viewfinderFps)
            else -> "—"
        }
    val boost = state.sensitivityBoost
    val boostValue = boost?.let { "×%.2f".format(it / 100f) } ?: "—"
    val boostActive = boost != null && boost > 100
    Row(
        Modifier
            .fillMaxWidth()
            .testTag(CaptureTestTags.EXPOSURE_MONITOR)
            .background(CaptureColors.Surface)
            .height(CaptureDimens.MonitorHeight),
        horizontalArrangement = Arrangement.spacedBy(6.dp, Alignment.CenterHorizontally),
        verticalAlignment = Alignment.CenterVertically
    ) {
        MonitorMetric("WB", wbValue)
        MonitorMetric("SS", state.exposureApplied.shutter?.displayLabel ?: "—")
        MonitorMetric("ISO", state.exposureApplied.iso?.displayLabel ?: "—")
        MonitorMetric(meterLabel, state.exposureApplied.evOrMeter?.displayLabel ?: "—")
        MonitorMetric("FOC", focusValue)
        MonitorMetric("FPS", fpsValue)
        MonitorMetric(
            "BST",
            boostValue,
            valueColor = if (boostActive) Color.White else Color.White.copy(alpha = .45f)
        )
    }
}

@Composable
internal fun MonitorMetric(label: String, value: String, valueColor: Color = Color.White) {
    // Tight boxes (no font padding) so CenterVertically is also optically
    // centered: caps-only mono glyphs otherwise sit above geometric center.
    val tight = TextStyle(platformStyle = PlatformTextStyle(includeFontPadding = false))
    Row(verticalAlignment = Alignment.CenterVertically) {
        MonitorLabel(label)
        Text(
            " $value",
            color = valueColor,
            fontFamily = CaptureMono,
            fontSize = 8.sp,
            lineHeight = 8.sp,
            fontWeight = FontWeight.Medium,
            style = tight
        )
    }
}

@Composable
internal fun MonitorLabel(label: String) {
    Text(
        label,
        color = CaptureColors.Muted.copy(alpha = .72f),
        fontFamily = CaptureMono,
        fontSize = 6.sp,
        lineHeight = 6.sp,
        fontWeight = FontWeight.Bold,
        letterSpacing = .4.sp,
        style = TextStyle(platformStyle = PlatformTextStyle(includeFontPadding = false))
    )
}
