package com.rawr.camera.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.Orientation as GestureOrientation
import androidx.compose.foundation.gestures.draggable
import androidx.compose.foundation.gestures.rememberDraggableState
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.systemGestureExclusion
import com.rawr.camera.ui.icons.Icons
import com.rawr.camera.ui.icons.outlined.ChevronLeft
import com.rawr.camera.ui.icons.outlined.Tune
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Shadow
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.onClick
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.rawr.camera.model.formatFilmStripValue

@Composable
internal fun StripParamsEntry(onOpen: () -> Unit, modifier: Modifier = Modifier) {
    val haptics = LocalCaptureHaptics.current
    Row(
        modifier
            .height(CaptureDimens.CompactParamRowHeight)
            .testTag(CaptureTestTags.FILM_STRIP_PARAMS)
            .captureClickable(shape = CaptureTextRippleShape) {
                haptics.selection()
                onOpen()
            }.semantics {
                contentDescription = "Parameters. Tap to adjust film settings."
                onClick {
                    haptics.selection()
                    onOpen()
                    true
                }
            }
            .padding(horizontal = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.Center
    ) {
        ContrastIcon(Icons.Outlined.Tune, Color.White.copy(alpha = .62f), 14.dp)
        Text(
            "PARAMS",
            color = Color.White.copy(alpha = .62f),
            fontFamily = CaptureMono,
            fontWeight = FontWeight.Bold,
            fontSize = 8.sp,
            letterSpacing = .4.sp,
            maxLines = 1,
            style = ViewfinderTextStyle,
            modifier = Modifier.padding(start = 3.dp)
        )
    }
}

@Composable
internal fun StripBackButton(onBack: () -> Unit, modifier: Modifier = Modifier) {
    val haptics = LocalCaptureHaptics.current
    Box(
        modifier
            .size(width = 32.dp, height = CaptureDimens.CompactParamRowHeight)
            .testTag(CaptureTestTags.FILM_STRIP_BACK)
            .captureClickable(shape = CaptureTextRippleShape) {
                haptics.selection()
                onBack()
            }.semantics {
                contentDescription = "Back"
                onClick {
                    haptics.selection()
                    onBack()
                    true
                }
            },
        contentAlignment = Alignment.Center
    ) {
        ContrastIcon(Icons.Outlined.ChevronLeft, Color.White.copy(alpha = .62f), 16.dp)
    }
}

/**
 * Leading scroll padding that parks the first param at the thumb (right)
 * edge with [StripDimens.ParkedVisibleCells] visible. Empty space sits on
 * the left; every cell can be scrolled over to the right for one-hand use.
 */
internal fun parkedParamsPadding(viewportWidth: Dp): Dp {
    val visible = StripDimens.ParamMinWidth * StripDimens.ParkedVisibleCells + StripDimens.ParamSpacing
    return (viewportWidth - visible).coerceAtLeast(0.dp)
}

@Composable
internal fun StripTwoLineItem(
    valueText: String,
    valueColor: Color,
    labelText: String,
    modifier: Modifier = Modifier
) {
    Column(
        modifier.padding(horizontal = 4.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center
    ) {
        Text(
            valueText,
            color = valueColor,
            fontFamily = CaptureMono,
            fontWeight = FontWeight.SemiBold,
            fontSize = 10.sp,
            lineHeight = 10.sp,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            style = ViewfinderTextStyle
        )
        Text(
            labelText,
            color = Color.White.copy(alpha = .62f),
            fontFamily = CaptureMono,
            fontWeight = FontWeight.Bold,
            fontSize = 8.sp,
            lineHeight = 8.sp,
            letterSpacing = .4.sp,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            style = ViewfinderTextStyle
        )
    }
}

/**
 * Two-line scrubbable film param (value above, short label below). VERTICAL
 * drag adjusts by spec steps with detent haptics (up = increase); double-tap
 * resets to the spec default. Vertical-only keeps edge items fully usable and
 * never fights the parent LazyRow's horizontal scroll. Zero-lag local readout
 * while dragging; committed value flows back through [value]/[display].
 */
@Composable
internal fun StripNumericItem(
    key: String,
    shortLabel: String,
    value: Float,
    display: String,
    isDefault: Boolean,
    defaultValue: Float,
    min: Float,
    max: Float,
    step: Float,
    decimals: Int,
    onScrub: (Float) -> Unit,
    onReset: () -> Unit,
    modifier: Modifier = Modifier,
    testTag: String = CaptureTestTags.filmParam(key),
    format: (Float) -> String = { formatFilmStripValue(it, min, decimals) },
    onDragStarted: () -> Unit = {},
    onDragStopped: () -> Unit = {}
) {
    val haptics = LocalCaptureHaptics.current
    var accumulated by remember(key) { mutableFloatStateOf(0f) }
    var gestureValue by remember(key) { mutableFloatStateOf(value) }
    var dragging by remember(key) { mutableStateOf(false) }
    var lastHaptic by remember(key) { mutableFloatStateOf(value) }
    val latestValue by rememberUpdatedState(value)
    val latestOnScrub by rememberUpdatedState(onScrub)
    val latestOnDragStarted by rememberUpdatedState(onDragStarted)
    val latestOnDragStopped by rememberUpdatedState(onDragStopped)
    val stepPx = with(LocalDensity.current) { 2.dp.toPx() }
    // Coarse enough to feel mechanical, fine enough to reach every detent:
    // at least 5 spec steps, at most ~40 detents across the range.
    val hapticThreshold = maxOf(step * 5f, (max - min) / 40f).takeIf { it > 0f } ?: step

    val shownDisplay = if (dragging) format(gestureValue) else display
    val shownDefault = if (dragging) {
        val epsilon = 0.5f / Math.pow(10.0, decimals.coerceIn(0, 3).toDouble()).toFloat()
        kotlin.math.abs(gestureValue - defaultValue) <= epsilon
    } else {
        isDefault
    }
    val valueColor = if (!shownDefault) CaptureColors.AccentSoft else Color.White.copy(alpha = .95f)

    val dragState = rememberDraggableState { delta ->
        // Vertical orientation reports down-positive; invert so up increases.
        accumulated += -delta
        val steps = (accumulated / stepPx).toInt()
        if (steps != 0) {
            val next = (gestureValue + steps * step).coerceIn(min, max)
            if (next != gestureValue) {
                gestureValue = next
                latestOnScrub(next)
                if (kotlin.math.abs(next - lastHaptic) >= hapticThreshold) {
                    lastHaptic = next
                    haptics.detent()
                }
            }
            accumulated -= steps * stepPx
        }
    }

    Box(
        modifier
            .widthIn(min = StripDimens.ParamMinWidth)
            .height(StripDimens.ParamRowHeight)
            .testTag(testTag)
            .systemGestureExclusion()
            .captureCombinedClickable(
                shape = CaptureTextRippleShape,
                onClick = {},
                onDoubleClick = {
                    gestureValue = latestValue
                    haptics.selection()
                    onReset()
                }
            ).draggable(
                state = dragState,
                orientation = GestureOrientation.Vertical,
                onDragStarted = {
                    accumulated = 0f
                    gestureValue = latestValue
                    lastHaptic = latestValue
                    dragging = true
                    latestOnDragStarted()
                },
                onDragStopped = {
                    dragging = false
                    latestOnDragStopped()
                }
            ).semantics {
                contentDescription = "$shortLabel $shownDisplay. Scrub vertically to adjust; double tap to reset."
            },
        contentAlignment = Alignment.Center
    ) {
        StripTwoLineItem(
            valueText = shownDisplay,
            valueColor = valueColor,
            labelText = shortLabel
        )
    }
}

/** Dark drop shadow keeps viewfinder glyphs legible on bright and dark scenes alike. */
private val ViewfinderGlyphShadow =
    Shadow(color = Color.Black.copy(alpha = .85f), offset = Offset(1.5f, 1.5f), blurRadius = 5f)

internal val ViewfinderTextStyle = TextStyle(shadow = ViewfinderGlyphShadow)

@Composable
internal fun ContrastIcon(icon: ImageVector, tint: Color, size: Dp, modifier: Modifier = Modifier) {
    Box(modifier.size(size)) {
        Icon(
            icon,
            contentDescription = null,
            tint = Color.Black.copy(alpha = .8f),
            modifier = Modifier.matchParentSize().offset(1.dp, 1.dp)
        )
        Icon(icon, contentDescription = null, tint = tint, modifier = Modifier.matchParentSize())
    }
}

@Composable
internal fun CompactParamShell(
    icon: ImageVector,
    title: String,
    valueText: String,
    locked: Boolean,
    testTag: String,
    semanticsDescription: String,
    modifier: Modifier = Modifier
) {
    val contentColor = if (locked) CaptureColors.Accent else Color.White.copy(alpha = .62f)
    val valueColor = if (locked) CaptureColors.AccentSoft else Color.White.copy(alpha = .95f)
    // Bare instrument styling (mirrors LensSelector): no card, no border —
    // icon + text only, accent carries the locked state.
    // Gesture exclusion keeps edge columns (ISO left, Focus right) scrubbing
    // instead of losing outward strokes to system navigation.
    Box(
        modifier
            .testTag(testTag)
            .systemGestureExclusion()
            .semantics { contentDescription = semanticsDescription },
        contentAlignment = Alignment.Center
    ) {
        // Two-line stacked cell: value above, icon + title below.
        // Never rotated: keeps its orientation in every device posture.
        Column(
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.Center,
            modifier = Modifier.padding(horizontal = 4.dp)
        ) {
            Text(
                valueText,
                color = valueColor,
                fontFamily = CaptureMono,
                fontWeight = FontWeight.SemiBold,
                fontSize = 10.sp,
                lineHeight = 12.sp,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                style = ViewfinderTextStyle
            )
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.Center
            ) {
                ContrastIcon(icon, contentColor, 14.dp)
                Text(
                    title,
                    color = contentColor,
                    fontFamily = CaptureMono,
                    fontWeight = FontWeight.Bold,
                    fontSize = 8.sp,
                    lineHeight = 10.sp,
                    letterSpacing = .4.sp,
                    maxLines = 1,
                    style = ViewfinderTextStyle,
                    modifier = Modifier.padding(start = 3.dp)
                )
                if (locked) {
                    Box(
                        Modifier
                            .padding(start = 3.dp)
                            .size(4.dp)
                            .background(CaptureColors.Accent, androidx.compose.foundation.shape.CircleShape)
                    )
                }
            }
        }
    }
}

@Composable
internal fun CompactSidePill(
    icon: ImageVector,
    label: String,
    active: Boolean,
    testTag: String,
    contentDescription: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    onLongClick: (() -> Unit)? = null,
    onLongClickLabel: String? = null
) {
    val haptics = LocalCaptureHaptics.current
    val contentColor = if (active) CaptureColors.Accent else Color.White.copy(alpha = .55f)
    // Bare toggle styling (mirrors LensSelector): icon + label only, no card.
    // Generous invisible touch target retained.
    Box(
        modifier
            .widthIn(min = CaptureDimens.CompactPillMinWidth)
            .height(CaptureDimens.CompactPillHeight)
            .testTag(testTag)
            .captureCombinedClickable(
                shape = CapturePillRippleShape,
                role = Role.Button,
                onClick = {
                    haptics.selection()
                    onClick()
                },
                onLongClickLabel = onLongClickLabel,
                onLongClick =
                    onLongClick?.let { action ->
                        {
                            haptics.longPress()
                            action()
                        }
                    }
            )
            .semantics {
                this.contentDescription = contentDescription
                role = Role.Switch
                onClick {
                    haptics.selection()
                    onClick()
                    true
                }
            },
        contentAlignment = Alignment.Center
    ) {
        Column(
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.Center,
            modifier = Modifier.padding(horizontal = 8.dp, vertical = 4.dp)
        ) {
            Icon(icon, contentDescription = null, tint = contentColor, modifier = Modifier.size(20.dp))
            Text(
                label,
                color = contentColor,
                fontFamily = CaptureMono,
                fontWeight = FontWeight.Bold,
                fontSize = 7.sp,
                letterSpacing = .5.sp,
                maxLines = 1
            )
        }
    }
}
