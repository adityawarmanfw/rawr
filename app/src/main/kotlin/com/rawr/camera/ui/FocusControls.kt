package com.rawr.camera.ui

import androidx.compose.animation.core.*
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.disabled
import androidx.compose.ui.semantics.onClick
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.rawr.camera.architecture.CancelSpotAe
import com.rawr.camera.architecture.CaptureDispatch
import com.rawr.camera.architecture.ClearAutofocus
import com.rawr.camera.architecture.FinishSpotAeMove
import com.rawr.camera.architecture.MoveSpotAe
import com.rawr.camera.architecture.OpenFocusSelector
import com.rawr.camera.architecture.Refocus
import com.rawr.camera.architecture.SetFocusMode
import com.rawr.camera.architecture.SetManualFocus
import com.rawr.camera.model.*

@Composable
internal fun AfTarget(state: CaptureUiState, dispatch: CaptureDispatch) {
    if (!state.capabilities.tapAfSupported) return
    // Full auto shows no box: the reticle only exists while a tap-AF request
    // is outstanding (Settling/Settled/Failed). Retapping it returns to
    // continuous AF; auto-dismiss does the same after a few seconds.
    if (state.focus.status == TargetStatus.Hidden) return
    val haptics = LocalCaptureHaptics.current
    val target = state.focus.target
    val color = targetColor(state.focus.status)
    val alpha = pulsingAlpha(state.focus.status)

    BoxWithConstraints(Modifier.fillMaxSize()) {
        Box(
            Modifier
                .offset(x = maxWidth * target.x - 22.dp, y = maxHeight * target.y - 22.dp)
                .size(44.dp)
                .testTag(CaptureTestTags.AF_TARGET)
                .semantics { contentDescription = "Autofocus target, ${state.focus.status.name.lowercase()}" }
                .pointerInput(state.focus.mode, state.focus.status) {
                    detectTapGestures(
                        onTap = {
                            haptics.selection()
                            if (state.focus.mode == FocusMode.Mf) {
                                dispatch(OpenFocusSelector)
                            } else if (state.focus.mode == FocusMode.Af) {
                                dispatch(ClearAutofocus)
                            } else {
                                dispatch(Refocus)
                            }
                        },
                        onLongPress = {
                            haptics.longPress()
                            dispatch(OpenFocusSelector)
                        }
                    )
                }.border(1.5.dp, color.copy(alpha = alpha), RoundedCornerShape(6.dp))
        ) {
            Canvas(Modifier.matchParentSize()) {
                drawLine(
                    color.copy(alpha = alpha),
                    Offset(size.width / 2, size.height * .24f),
                    Offset(
                        size.width / 2,
                        size.height * .76f
                    ),
                    1f
                )
                drawLine(
                    color.copy(alpha = alpha),
                    Offset(size.width * .24f, size.height / 2),
                    Offset(
                        size.width * .76f,
                        size.height / 2
                    ),
                    1f
                )
            }
            if (state.focus.mode == FocusMode.AfLock) {
                Text(
                    "L",
                    color = color,
                    fontFamily = CaptureMono,
                    fontWeight = FontWeight.ExtraBold,
                    fontSize = 8.sp,
                    modifier = Modifier.align(Alignment.TopEnd).padding(2.dp).uprightInLandscape(state.orientation)
                )
            }
        }
    }
}

/**
 * Face-priority boxes. Display-only (no gesture handling — taps pass through
 * to tap-to-focus below): in full auto the primary face also steers the native
 * AF region, so faces genuinely drive focus. A tap overrides until cleared.
 */
@Composable
internal fun FaceBoxes(faces: List<com.rawr.camera.model.FaceDetection>) {
    if (faces.isEmpty()) return
    BoxWithConstraints(
        Modifier
            .fillMaxSize()
            .testTag(CaptureTestTags.FACE_BOXES)
            .semantics { contentDescription = "Detected faces, ${faces.size}" }
    ) {
        faces.forEachIndexed { index, face ->
            val primary = index == 0
            val color = if (primary) CaptureColors.Success else Color.White.copy(alpha = .6f)
            Box(
                Modifier
                    .offset(x = maxWidth * face.x, y = maxHeight * face.y)
                    .size(width = maxWidth * face.w, height = maxHeight * face.h)
                    .testTag(CaptureTestTags.faceBox(index))
            ) {
                Canvas(Modifier.matchParentSize()) {
                    val corner = (size.minDimension * .28f).coerceAtLeast(8f)
                    val stroke = 3f
                    // Top-left
                    drawLine(color, Offset(0f, corner), Offset(0f, 0f), stroke)
                    drawLine(color, Offset(0f, 0f), Offset(corner, 0f), stroke)
                    // Top-right
                    drawLine(color, Offset(size.width - corner, 0f), Offset(size.width, 0f), stroke)
                    drawLine(color, Offset(size.width, 0f), Offset(size.width, corner), stroke)
                    // Bottom-left
                    drawLine(color, Offset(0f, size.height - corner), Offset(0f, size.height), stroke)
                    drawLine(color, Offset(0f, size.height), Offset(corner, size.height), stroke)
                    // Bottom-right
                    drawLine(
                        color,
                        Offset(size.width - corner, size.height),
                        Offset(size.width, size.height),
                        stroke
                    )
                    drawLine(
                        color,
                        Offset(size.width, size.height - corner),
                        Offset(size.width, size.height),
                        stroke
                    )
                }
            }
        }
    }
}

@Composable
internal fun SpotAeTarget(state: CaptureUiState, dispatch: CaptureDispatch) {
    if (!state.spotAe.active || state.exposureControl.mode != ExposureMode.Auto) return
    val haptics = LocalCaptureHaptics.current
    val target = state.spotAe.target
    val color = targetColor(state.spotAe.status)
    val alpha = pulsingAlpha(state.spotAe.status)
    val latestTarget by rememberUpdatedState(target)

    BoxWithConstraints(Modifier.fillMaxSize()) {
        var surfaceSize by remember { mutableStateOf(androidx.compose.ui.unit.IntSize.Zero) }
        var dragTarget by remember { mutableStateOf(target) }

        // Keep the local gesture accumulator synchronized when not actively dragging.
        LaunchedEffect(target) { dragTarget = target }

        Box(
            Modifier
                .matchParentSize()
                .onSizeChanged { surfaceSize = it }
        )

        Box(
            Modifier
                .offset(x = maxWidth * target.x - 26.dp, y = maxHeight * target.y - 26.dp)
                // 52dp gesture target around the 44dp visual target makes dragging reliable
                // without making the AE graphic visually larger than the prototype.
                .size(52.dp)
                .testTag(CaptureTestTags.SPOT_AE_TARGET)
                .semantics {
                    contentDescription =
                        "Spot AE target, ${state.spotAe.status.name.lowercase()}. Tap to cancel; draggable."
                }.pointerInput(state.spotAe.active) {
                    detectTapGestures(
                        onTap = {
                            haptics.selection()
                            dispatch(CancelSpotAe)
                        }
                    )
                }.pointerInput(surfaceSize) {
                    detectDragGestures(
                        onDragStart = {
                            haptics.selection()
                            dragTarget = latestTarget
                        },
                        onDrag = { change, drag ->
                            change.consume()
                            if (surfaceSize.width > 0 && surfaceSize.height > 0) {
                                dragTarget =
                                    NormalizedPoint(
                                        x = dragTarget.x + drag.x / surfaceSize.width,
                                        y = dragTarget.y + drag.y / surfaceSize.height
                                    ).clamped()
                                dispatch(MoveSpotAe(dragTarget))
                            }
                        },
                        onDragEnd = { dispatch(FinishSpotAeMove) },
                        onDragCancel = { dispatch(FinishSpotAeMove) }
                    )
                },
            contentAlignment = Alignment.Center
        ) {
            Box(
                Modifier
                    .size(44.dp)
                    .border(2.dp, color.copy(alpha = alpha), CircleShape),
                contentAlignment = Alignment.Center
            ) {
                Text(
                    "AE",
                    color = color.copy(alpha = alpha),
                    fontFamily = CaptureMono,
                    fontWeight = FontWeight.ExtraBold,
                    fontSize = 9.sp,
                    modifier = Modifier.uprightInLandscape(state.orientation)
                )
            }
        }
    }
}

internal fun targetColor(status: TargetStatus): Color = when (status) {
    TargetStatus.Settling -> CaptureColors.Accent
    TargetStatus.Settled -> CaptureColors.Success
    TargetStatus.Failed -> CaptureColors.Danger
    TargetStatus.Hidden -> Color.Transparent
}

@Composable
private fun pulsingAlpha(status: TargetStatus): Float {
    if (status != TargetStatus.Settling) return 1f
    val transition = rememberInfiniteTransition(label = "targetPulse")
    return transition
        .animateFloat(
            initialValue = 1f,
            targetValue = .45f,
            animationSpec = infiniteRepeatable(tween(400), RepeatMode.Reverse),
            label = "targetAlpha"
        ).value
}

@Composable
internal fun FocusBottomControls(state: CaptureUiState, dispatch: CaptureDispatch, modifier: Modifier = Modifier) {
    Column(modifier, verticalArrangement = Arrangement.spacedBy(CaptureDimens.ControlGap)) {
        FocusModeSelector(
            selected = state.focus.mode,
            mfSupported = state.capabilities.manualFocus.supported,
            orientation = state.orientation,
            dispatch = dispatch,
            modifier = Modifier.fillMaxWidth().height(CaptureDimens.QuickRowHeight)
        )
        MfExposureScrub(state, dispatch, Modifier.fillMaxWidth())
    }
}

/**
 * Segmented focus-mode bar using the exact same material treatment as the
 * AMSI exposure-mode selector: single clipped bar, shared control surface,
 * hairline dividers, accent-tinted active cell.
 */
@Composable
private fun FocusModeSelector(
    selected: FocusMode,
    mfSupported: Boolean,
    orientation: Orientation,
    dispatch: CaptureDispatch,
    modifier: Modifier = Modifier
) {
    val haptics = LocalCaptureHaptics.current
    val shape = RoundedCornerShape(CaptureDimens.GlassControlRadius)
    Row(
        modifier
            .testTag(CaptureTestTags.FOCUS_MODE_BAR)
            .clip(shape)
            .captureControlSurface(shape = shape, textureStrength = .75f),
        verticalAlignment = Alignment.CenterVertically
    ) {
        FocusMode.entries.forEachIndexed { i, mode ->
            val enabled = mode != FocusMode.Mf || mfSupported
            val active = mode == selected
            Box(
                Modifier
                    .weight(1f)
                    .fillMaxHeight()
                    .testTag(CaptureTestTags.focusMode(mode.name))
                    .then(
                        if (active) {
                            Modifier.background(CaptureColors.Accent.copy(alpha = .075f))
                        } else {
                            Modifier
                        }
                    )
                    .then(
                        if (enabled) {
                            Modifier.pointerInput(mode) {
                                detectTapGestures {
                                    haptics.selection()
                                    dispatch(SetFocusMode(mode))
                                }
                            }
                        } else {
                            Modifier
                        }
                    ).semantics {
                        contentDescription = "Focus mode ${mode.name}"
                        role = Role.RadioButton
                        this.selected = active
                        if (!enabled) disabled()
                        onClick {
                            if (!enabled) return@onClick false
                            haptics.selection()
                            dispatch(SetFocusMode(mode))
                            true
                        }
                    },
                contentAlignment = Alignment.Center
            ) {
                Text(
                    if (mode == FocusMode.AfLock) "AF LOCK" else mode.name.uppercase(),
                    color =
                        when {
                            active -> CaptureColors.Accent
                            enabled -> Color.White.copy(alpha = .48f)
                            else -> Color.White.copy(alpha = .16f)
                        },
                    fontFamily = CaptureMono,
                    fontWeight = FontWeight.SemiBold,
                    fontSize = 10.sp,
                    modifier = Modifier.uprightInLandscape(orientation)
                )
            }
            if (i != FocusMode.entries.lastIndex) {
                Box(
                    Modifier
                        .width(.5.dp)
                        .fillMaxHeight(.48f)
                        .background(Color.White.copy(alpha = .055f))
                )
            }
        }
    }
}

/** Rail share at each end that snaps a tap to NEAR / ∞. */
private const val MF_RAIL_END_ZONE = .06f
/** Drag positions this close to a ladder anchor snap onto it. */
private const val MF_RAIL_ANCHOR_SNAP = .015f

/**
 * Manual-focus rail using the exact same reference-rail visuals as the
 * EV/ISO/Shutter scrubs ([ReferenceExposureRail], [ExposureRailMarks]).
 * Drag is continuous and relative: the gesture starts at the current lens
 * distance and applies deltas, so there is no jump to the finger position.
 */
@Composable
private fun MfExposureScrub(state: CaptureUiState, dispatch: CaptureDispatch, modifier: Modifier = Modifier) {
    val haptics = LocalCaptureHaptics.current
    val capability = state.capabilities.manualFocus
    // The rail stays interactive outside MF: touching it auto-enters MF
    // (controller flips the mode on SetManualFocus). Only truly disabled when
    // the camera has no manual focus.
    val interactive = capability.supported
    val mf = state.focus.mfNormalized
    val latestMf by rememberUpdatedState(mf)
    val latestInMf by rememberUpdatedState(state.focus.mode == FocusMode.Mf)
    // Drag base when auto-entering from AF: the live lens position, so the first
    // movement continues from current focus instead of jumping to a stale value.
    val latestEntryBase by rememberUpdatedState(
        state.focus.appliedFocusDiopters?.let {
            mfNormalizedFromDiopters(it, capability.minimumFocusDistance)
        } ?: mf
    )
    val readout = state.focusDistanceLabel()
    val ladder = FocusLadder.forCapability(capability)

    var railWidthPx by remember { mutableIntStateOf(1) }
    var rawTrackOffsetPx by remember { mutableFloatStateOf(0f) }
    var dragging by remember { mutableStateOf(false) }
    var dragStartValue by remember { mutableFloatStateOf(mf) }
    var accumulatedPx by remember { mutableFloatStateOf(0f) }
    var lastAnchor by remember { mutableStateOf<FocusAnchor?>(null) }
    val animatedTrackOffsetPx by animateFloatAsState(
        targetValue = if (dragging) rawTrackOffsetPx else 0f,
        animationSpec = if (dragging) snap() else spring(dampingRatio = .78f, stiffness = 720f),
        label = "mf-track-snap"
    )
    // Same inverted presentation as the exposure rails: dragging right moves toward
    // far/∞ while the numbered scale travels left underneath the fixed center index.
    val renderedTrackOffsetPx = -(if (dragging) rawTrackOffsetPx else animatedTrackOffsetPx)

    // Detents fire when landing on a ladder anchor (NEAR / hyperfocal / ∞),
    // the same stops the compact focus button snaps to.
    fun setFocus(value: Float) {
        if (!interactive) return
        val clamped = value.coerceIn(capability.minNormalized, capability.maxNormalized)
        val anchor = ladder?.anchorNear(clamped, MF_RAIL_ANCHOR_SNAP)
        if (anchor?.anchor != lastAnchor) {
            lastAnchor = anchor?.anchor
            if (anchor != null) haptics.detent()
        }
        dispatch(SetManualFocus(anchor?.normalized ?: clamped))
    }

    fun elasticVisual(accumulated: Float, start: Float, widthPx: Float): Float {
        if (widthPx <= 0f) return 0f
        val unclamped = start + accumulated / widthPx
        val clamped =
            unclamped.coerceIn(capability.minNormalized, capability.maxNormalized)
        val inRangePx = (clamped - start) * widthPx
        val overshoot = accumulated - inRangePx
        return inRangePx + overshoot * .18f
    }

    Box(
        modifier
            .height(CaptureDimens.ExposureControlHeight)
            .testTag(CaptureTestTags.FOCUS_SCRUB)
            .clip(RoundedCornerShape(CaptureDimens.ExposureCornerRadius))
            .captureControlSurface(
                shape = RoundedCornerShape(CaptureDimens.ExposureCornerRadius),
                textureStrength = .9f
            )
            .then(if (interactive) Modifier else Modifier.graphicsLayer(alpha = .45f))
            .then(
                if (interactive) {
                    Modifier.pointerInput(railWidthPx) {
                        detectTapGestures(
                            onTap = { offset ->
                                if (railWidthPx > 0) {
                                    val fraction = offset.x / railWidthPx
                                    setFocus(
                                        when {
                                            fraction >= 1f - MF_RAIL_END_ZONE -> 1f
                                            fraction <= MF_RAIL_END_ZONE -> 0f
                                            else -> fraction
                                        }
                                    )
                                }
                            }
                        )
                    }
                } else {
                    Modifier
                }
            )
            .then(
                if (interactive) {
                    Modifier.pointerInput(railWidthPx) {
                        detectDragGestures(
                            onDragStart = {
                                dragging = true
                                // Auto-enter base: live lens position when coming
                                // from AF, current value when already in MF.
                                // Mode is read via updated-state because this
                                // pointerInput block does not restart on mode taps.
                                dragStartValue =
                                    if (latestInMf) latestMf else latestEntryBase
                                accumulatedPx = 0f
                                rawTrackOffsetPx = 0f
                                lastAnchor = ladder?.anchorNear(dragStartValue, MF_RAIL_ANCHOR_SNAP)?.anchor
                            },
                            onDrag = { change, drag ->
                                change.consume()
                                if (railWidthPx > 0) {
                                    accumulatedPx += drag.x
                                    setFocus(dragStartValue + accumulatedPx / railWidthPx)
                                    rawTrackOffsetPx =
                                        elasticVisual(accumulatedPx, dragStartValue, railWidthPx.toFloat())
                                }
                            },
                            onDragEnd = {
                                dragging = false
                                rawTrackOffsetPx = 0f
                            },
                            onDragCancel = {
                                dragging = false
                                rawTrackOffsetPx = 0f
                            }
                        )
                    }
                } else {
                    Modifier
                }
            )
            .onSizeChanged { railWidthPx = it.width.coerceAtLeast(1) }
            .semantics {
                contentDescription =
                    "Manual focus scrub. $readout. ${if (state.orientation == Orientation.Portrait) "Right" else "Up"} is far. Drag or tap to focus manually."
            }
    ) {
        ReferenceExposureRail(
            title = "FOCUS",
            current = readout,
            previous = "NEAR",
            next = "∞",
            atMinimum = mf <= capability.minNormalized + 1e-6f,
            atMaximum = mf >= capability.maxNormalized - 1e-6f,
            trackOffsetPx = renderedTrackOffsetPx,
            modifier = Modifier.matchParentSize()
        )
    }
}
