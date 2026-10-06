package com.rawr.camera.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.unit.dp

/** Recording metrics in the same fixed-height, labeled format as the EV monitor. */
@Composable
internal fun VideoTimingMonitor(state: VideoControlState) {
    val timing = state.timing
    Row(
        Modifier.fillMaxWidth()
            .testTag(CaptureTestTags.VIDEO_TIMING_MONITOR)
            .background(CaptureColors.Surface)
            .height(CaptureDimens.MonitorHeight),
        horizontalArrangement = Arrangement.spacedBy(6.dp, Alignment.CenterHorizontally),
        verticalAlignment = Alignment.CenterVertically
    ) {
        MonitorMetric("FPS", timing?.let { "${state.fps}/${"%.1f".format(it.actualFps)}" } ?: "${state.fps}/—")
        MonitorMetric("DROP", timing?.let { t -> t.dropReason?.let { "${t.dropped}·$it" } ?: "${t.dropped}" } ?: "—",
            valueColor = if (timing != null && timing.dropped > 0) CaptureColors.Danger else Color.White)
        MonitorMetric("SHORT", timing?.shortfall?.toString() ?: "—")
        // Average/recent peak: a drop comes from one slow frame, which the average hides.
        MonitorMetric("GPU", timing?.let { "%.0f/%.0fms".format(it.gpuMs, it.gpuPeakMs) } ?: "—")
        MonitorMetric("LAT", timing?.let { "%.0f/%.0fms".format(it.encoderMs, it.encoderPeakMs) } ?: "—")
    }
}
