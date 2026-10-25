package com.rawr.camera.ui

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.rawr.camera.model.ExposureFormat
import com.rawr.camera.model.GridMode
import com.rawr.camera.model.LongExposureUiState
import com.rawr.camera.model.Orientation
import com.rawr.camera.model.OverlayMode

/**
 * Pure viewfinder overlay renderables extracted from CaptureViewfinder.
 *
 * Placeholder fixture and grid/monitoring overlays. Gesture handling, panel
 * placement and state mapping stay in CaptureViewfinder.
 */

@Composable
internal fun ViewfinderFixture(modifier: Modifier) {
    Canvas(modifier) {
        drawRect(
            Brush.linearGradient(
                colors =
                    listOf(
                        Color(0xFF111820),
                        Color(0xFF24313B),
                        Color(0xFF56616A),
                        Color(0xFF20282E),
                        Color(0xFF101317)
                    ),
                start = Offset.Zero,
                end = Offset(size.width, size.height)
            )
        )
        drawCircle(
            color = Color(0xFF59636C),
            radius = size.minDimension * .10f,
            center = Offset(size.width * .68f, size.height * .28f)
        )
        drawOval(
            brush =
                Brush.radialGradient(
                    colors = listOf(Color.Transparent, Color.Black.copy(alpha = .42f)),
                    center = Offset(size.width / 2f, size.height / 2f),
                    radius = size.maxDimension * .62f
                ),
            topLeft = Offset.Zero,
            size = Size(size.width, size.height)
        )
    }
}

@Composable
internal fun GridOverlay(mode: GridMode, modifier: Modifier) {
    if (mode == GridMode.Off) return
    Canvas(modifier) {
        val color = Color.White.copy(alpha = .20f)
        when (mode) {
            GridMode.Thirds -> {
                listOf(1f / 3f, 2f / 3f).forEach { fraction ->
                    drawLine(color, Offset(size.width * fraction, 0f), Offset(size.width * fraction, size.height), 1f)
                    drawLine(color, Offset(0f, size.height * fraction), Offset(size.width, size.height * fraction), 1f)
                }
            }

            GridMode.FourByFour -> {
                (1..3).forEach { index ->
                    val fraction = index / 4f
                    drawLine(
                        color.copy(alpha = .15f),
                        Offset(size.width * fraction, 0f),
                        Offset(size.width * fraction, size.height),
                        1f
                    )
                    drawLine(
                        color.copy(alpha = .15f),
                        Offset(0f, size.height * fraction),
                        Offset(
                            size.width,
                            size.height * fraction
                        ),
                        1f
                    )
                }
            }

            GridMode.Cross -> {
                drawLine(
                    color,
                    Offset(size.width / 2f, size.height * .2f),
                    Offset(size.width / 2f, size.height * .8f),
                    1f
                )
                drawLine(
                    color,
                    Offset(size.width * .2f, size.height / 2f),
                    Offset(size.width * .8f, size.height / 2f),
                    1f
                )
            }

            GridMode.Off -> {
                Unit
            }
        }
    }
}

@Composable
internal fun MonitoringOverlay(armed: Set<OverlayMode>, falseColor: Boolean, modifier: Modifier) {
    if (armed.isEmpty() && !falseColor) return
    Canvas(modifier) {
        if (OverlayMode.Peaking in armed) {
            for (i in -size.height.toInt()..size.width.toInt() step 42) {
                drawLine(
                    CaptureColors.Accent.copy(alpha = .38f),
                    Offset(i.toFloat(), 0f),
                    Offset(i + size.height, size.height),
                    2f
                )
            }
        }

        if (OverlayMode.RawHighlights in armed) {
            drawCircle(
                CaptureColors.Danger.copy(alpha = .52f),
                size.minDimension * .07f,
                Offset(size.width * .7f, size.height * .23f)
            )
        }

        if (OverlayMode.TonemapShadows in armed) {
            // B/W diagonal zebra placeholder for the tonemap-shadow overlay.
            for (i in -size.height.toInt()..size.width.toInt() step 24) {
                drawLine(
                    Color.White.copy(alpha = .30f),
                    Offset(i.toFloat(), 0f),
                    Offset(i + size.height, size.height),
                    6f
                )
            }
            drawRect(Color.Black.copy(alpha = .18f))
        }

        if (falseColor) {
            drawRect(Color.Blue.copy(alpha = .16f))
            drawCircle(
                Color.Green.copy(alpha = .18f),
                size.minDimension * .35f,
                Offset(
                    size.width * .4f,
                    size.height * .55f
                )
            )
            drawCircle(
                Color.Red.copy(alpha = .18f),
                size.minDimension * .28f,
                Offset(
                    size.width * .76f,
                    size.height * .35f
                )
            )
        }
    }
}

/**
 * Centered self-timer countdown readout. Covers the viewfinder so taps
 * cancel the run instead of moving focus; palettes and the bottom slot stay
 * above/below via z-order at the call site.
 */
@Composable
internal fun SelfTimerCountdownOverlay(
    remainingMs: Long,
    orientation: Orientation,
    onCancel: () -> Unit,
    modifier: Modifier = Modifier
) {
    val seconds = ((remainingMs + 999) / 1000).toInt().coerceAtLeast(1)
    Box(
        modifier
            .fillMaxSize()
            .testTag(CaptureTestTags.SELF_TIMER_COUNTDOWN)
            .captureClickable(shape = CircleShape, onClick = onCancel)
            .semantics {
                contentDescription = "Self timer $seconds seconds remaining. Tap to cancel."
            },
        contentAlignment = Alignment.Center
    ) {
        Text(
            "$seconds",
            color = Color.White,
            fontFamily = CaptureMono,
            fontWeight = FontWeight.Bold,
            fontSize = 72.sp,
            maxLines = 1,
            style = ViewfinderTextStyle,
            modifier = Modifier.uprightInLandscape(orientation)
        )
    }
}

/**
 * Brief notice at the top of the viewfinder when HDR+ Bracketed becomes
 * active (and on entering the camera with it on): the burst is exposed darker
 * and lifted back, and frames follow the shutter. Fades out on its own.
 */
@Composable
internal fun HdrBracketedNotice(active: Boolean, orientation: Orientation, modifier: Modifier = Modifier) {
    var visible by remember { mutableStateOf(false) }
    LaunchedEffect(active) {
        visible = active
        if (active) {
            kotlinx.coroutines.delay(HDR_BRACKETED_NOTICE_MS)
            visible = false
        }
    }
    AnimatedVisibility(
        visible = visible,
        enter = fadeIn(tween(250)),
        exit = fadeOut(tween(600)),
        modifier = modifier
    ) {
        Text(
            "HDR+ Bracketed is active. The app will keep highlights from clipping and take a few " +
                "extra frames after the shutter, so hold still a moment.",
            color = Color.White,
            fontSize = 13.sp,
            textAlign = TextAlign.Center,
            style = ViewfinderTextStyle,
            modifier = Modifier
                .uprightInLandscape(orientation)
                .widthIn(max = 320.dp)
                .background(Color.Black.copy(alpha = .45f), RoundedCornerShape(12.dp))
                .padding(horizontal = 14.dp, vertical = 8.dp)
                .testTag(CaptureTestTags.HDR_BRACKETED_NOTICE)
        )
    }
}

private const val HDR_BRACKETED_NOTICE_MS = 4_000L

private const val FULL_SIZE_FALLBACK_NOTICE_MS = 3_000L

/** Longest shutter multiframe still stacks (native kStackShutterMaxNs). */
internal const val LONG_EXPOSURE_STACK_MAX_NS = 1_100_000_000L

/**
 * The one capture status pill, in priority order: a long exposure's countdown
 * (or "Hold still" while untimed), "Hold still" until post-shutter frames are
 * in (felt as a tick at the end), "Restoring preview…", a moment's note when
 * the camera skipped full res (the counters went up), and before the press
 * what a long exposure will be (one frame, or a stacked burst up to
 * [LONG_EXPOSURE_STACK_MAX_NS] with multiframe on).
 */
@Composable
internal fun CaptureStatusNotice(
    longExposure: LongExposureUiState,
    holdActive: Boolean,
    restoringPreview: Boolean,
    stacked: Boolean,
    orientation: Orientation,
    modifier: Modifier = Modifier,
    fullSizeBinnedStills: Long = 0L,
    fullSizeBurstsToSingle: Long = 0L
) {
    var seenStills by remember { mutableLongStateOf(fullSizeBinnedStills) }
    var seenBursts by remember { mutableLongStateOf(fullSizeBurstsToSingle) }
    var fallback by remember { mutableStateOf<String?>(null) }
    LaunchedEffect(fullSizeBinnedStills, fullSizeBurstsToSingle) {
        val text = when {
            fullSizeBinnedStills > seenStills -> "Camera skipped full res · saved at 12 MP"
            fullSizeBurstsToSingle > seenBursts -> "Camera skipped full res · took a single shot"
            else -> return@LaunchedEffect
        }
        seenStills = fullSizeBinnedStills
        seenBursts = fullSizeBurstsToSingle
        fallback = text
        kotlinx.coroutines.delay(FULL_SIZE_FALLBACK_NOTICE_MS)
        fallback = null
    }
    var now by remember { mutableLongStateOf(android.os.SystemClock.uptimeMillis()) }
    if (longExposure.active) {
        LaunchedEffect(longExposure.startMs, longExposure.activeNs) {
            while (true) {
                now = android.os.SystemClock.uptimeMillis()
                kotlinx.coroutines.delay(100)
            }
        }
    }
    val text = when {
        // A timed hold (a long exposure or stacked burst) counts down; a short
        // wait (a full-size still waiting for the sensor switch) holds still.
        longExposure.active -> {
            val remainingMs = longExposure.remainingMs(now) ?: 0L
            if ((longExposure.activeNs >= 1_000_000_000L || longExposure.totalMs >= 1_000L) && remainingMs > 0L)
                "%.1f s".format(remainingMs / 1000.0)
            else "Hold still"
        }
        holdActive -> "Hold still"
        restoringPreview -> "Restoring preview…"
        fallback != null -> fallback
        longExposure.deferredNs > 0L -> buildString {
            append("Long exposure · ")
            append(ExposureFormat.formatShutterNs(longExposure.deferredNs))
            append(if (stacked) " · stacked" else " · 1 frame")
            if (longExposure.lastHonored == 0) append("\nCamera shortened the last one")
        }
        else -> null
    }
    // The fade-out keeps the last text: an emptied pill would flash.
    var shown by remember { mutableStateOf("") }
    if (text != null) shown = text
    AnimatedVisibility(visible = text != null, enter = fadeIn(tween(80)), exit = fadeOut(tween(250)), modifier = modifier) {
        Text(
            shown,
            color = Color.White,
            fontSize = 14.sp,
            textAlign = TextAlign.Center,
            style = ViewfinderTextStyle,
            modifier = Modifier
                .background(Color.Black.copy(alpha = .45f), RoundedCornerShape(12.dp))
                .padding(horizontal = 16.dp, vertical = 8.dp)
                .testTag(CaptureTestTags.BRACKET_HOLD_NOTICE)
        )
    }
}
