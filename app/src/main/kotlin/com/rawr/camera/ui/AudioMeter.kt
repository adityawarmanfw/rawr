package com.rawr.camera.ui

import android.Manifest
import android.content.pm.PackageManager
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.LifecycleResumeEffect
import com.rawr.camera.video.AudioLevelMonitor
import com.rawr.camera.video.VideoAudioInput
import kotlin.math.log10
import kotlin.math.max

private const val FLOOR_DB = -60f
private const val PEAK_HOLD_NS = 1_500_000_000L
private const val RELEASE_DB_PER_S = 20f
private val TICKS_DB = floatArrayOf(-40f, -20f, -12f, -6f)
private val Amber = Color(0xFFF2C14E)

private fun dbfs(linear: Float): Float = if (linear <= 0f) FLOOR_DB else max(FLOOR_DB, 20f * log10(linear))

/** Per-channel meter ballistics: instant attack, 20 dB/s release, 1.5 s peak hold. */
private class ChannelBallistics {
    var barDb = FLOOR_DB
    var holdDb = FLOOR_DB
    var holdUntilNs = 0L

    fun update(peak: Float, nowNs: Long, dtS: Float) {
        val peakDb = dbfs(peak)
        barDb = max(peakDb, barDb - RELEASE_DB_PER_S * dtS)
        if (peakDb >= holdDb || nowNs > holdUntilNs) {
            holdDb = peakDb
            holdUntilNs = nowNs + PEAK_HOLD_NS
        }
    }
}

/**
 * Live L/R level meter for the video monitor strip, before and during recording. Holds the
 * shared microphone while the screen is resumed. The clip lamp latches until tapped
 * and clears at each record start.
 */
@Composable
internal fun AudioMeter(input: VideoAudioInput, recording: Boolean, modifier: Modifier = Modifier) {
    val context = LocalContext.current
    var monitor by remember { mutableStateOf<AudioLevelMonitor?>(null) }
    // Resume, not start: the permission dialog only pauses, so a grant opens the meter on return.
    // Source, route and channels need a new stream; gain and low-cut apply live below.
    LifecycleResumeEffect(input.channels, input.preset, input.route) {
        if (context.checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED) {
            @Suppress("MissingPermission")
            monitor = AudioLevelMonitor.open(context, input)
        }
        onPauseOrDispose {
            monitor?.close()
            monitor = null
        }
    }
    val active = monitor ?: return
    LaunchedEffect(active, input.gainDb, input.lowCutHz) { active.setProcessing(input.gainDb, input.lowCutHz) }
    val channels = remember { arrayOf(ChannelBallistics(), ChannelBallistics()) }
    var frame by remember { mutableLongStateOf(0L) }
    var channelCount by remember { mutableStateOf(2) }
    var seenClips by remember { mutableLongStateOf(-1L) }
    var clipLatched by remember { mutableStateOf(false) }
    // Each take starts with a clear lamp; it then reports clipping within that take.
    LaunchedEffect(recording) { if (recording) clipLatched = false }
    LaunchedEffect(active) {
        var lastNs = 0L
        while (true) {
            withFrameNanos { now ->
                val levels = active.read() ?: return@withFrameNanos
                val dt = if (lastNs == 0L) 0f else (now - lastNs) / 1e9f
                lastNs = now
                channelCount = levels.channels.coerceIn(1, 2)
                for (c in 0 until channelCount) channels[c].update(levels.peak[c], now, dt)
                val clips = levels.inputClips + levels.outputClips
                if (seenClips >= 0 && clips > seenClips) clipLatched = true
                seenClips = clips
                frame = now
            }
        }
    }
    Row(
        modifier
            .testTag(CaptureTestTags.VIDEO_AUDIO_METER)
            .semantics { contentDescription = if (clipLatched) "Audio level meter, clipped" else "Audio level meter" }
            .clickable { clipLatched = false },
        verticalAlignment = Alignment.CenterVertically
    ) {
        MonitorLabel("MIC")
        Canvas(Modifier.padding(start = 3.dp).width(64.dp).height(if (channelCount == 2) 7.dp else 3.dp)) {
            frame // Redraw on every polled frame.
            val gap = 1.dp.toPx()
            val bar = (size.height - gap * (channelCount - 1)) / channelCount
            fun x(db: Float) = size.width * (db - FLOOR_DB) / -FLOOR_DB
            val amberX = x(-18f)
            val redX = x(-6f)
            for (c in 0 until channelCount) {
                val top = c * (bar + gap)
                val meter = channels[c]
                drawRect(Color.White.copy(alpha = .08f), Offset(0f, top), Size(size.width, bar))
                val end = x(meter.barDb)
                drawRect(CaptureColors.Success, Offset(0f, top), Size(minOf(end, amberX), bar))
                if (end > amberX) drawRect(Amber, Offset(amberX, top), Size(minOf(end, redX) - amberX, bar))
                if (end > redX) drawRect(CaptureColors.Danger, Offset(redX, top), Size(end - redX, bar))
                if (meter.holdDb > FLOOR_DB)
                    drawRect(Color.White, Offset(x(meter.holdDb) - 1f, top), Size(2f, bar))
            }
            for (tick in TICKS_DB) drawRect(CaptureColors.Surface, Offset(x(tick), 0f), Size(1f, size.height))
        }
        Canvas(Modifier.padding(start = 3.dp).width(5.dp).height(5.dp)) {
            drawCircle(if (clipLatched) CaptureColors.Danger else Color.White.copy(alpha = .18f))
        }
    }
}
