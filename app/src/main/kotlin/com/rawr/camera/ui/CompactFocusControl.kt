package com.rawr.camera.ui

import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.pointer.pointerInput
import com.rawr.camera.architecture.CaptureDispatch
import com.rawr.camera.architecture.SetFocusMode
import com.rawr.camera.architecture.SetManualFocus
import com.rawr.camera.model.CaptureUiState
import com.rawr.camera.model.FocusLadder
import com.rawr.camera.model.FocusMode
import com.rawr.camera.model.FocusScrubPosition
import com.rawr.camera.model.focusDistanceLabel
import com.rawr.camera.ui.icons.Icons
import com.rawr.camera.ui.icons.outlined.CenterFocusStrong

/**
 * Compact focus scrub, same gesture grammar as SS/ISO: drag enters MF from
 * the exact live lens position and scrubs the [FocusLadder] continuously
 * (right/up = farther),
 * long-press locks AF (AF-L), double-tap returns to continuous AF.
 */
@Composable
internal fun CompactFocusButton(state: CaptureUiState, dispatch: CaptureDispatch, modifier: Modifier = Modifier) {
    val haptics = LocalCaptureHaptics.current
    val capability = state.capabilities.manualFocus
    val ladder = FocusLadder.forCapability(capability)
    val mode = state.focus.mode
    val latestMode by rememberUpdatedState(mode)
    // Seed: the requested value once in MF, otherwise the live lens position so
    // grabbing the control never jumps focus.
    val seedDiopters =
        if (mode == FocusMode.Mf || ladder == null) {
            ladder?.dioptersOf(state.focus.mfNormalized) ?: 0f
        } else {
            state.focus.appliedFocusDiopters?.takeIf { it.isFinite() } ?: ladder.dioptersOf(state.focus.mfNormalized)
        }
    val latestSeedDiopters by rememberUpdatedState(seedDiopters)
    val title = when (mode) {
        FocusMode.Af -> "AF"
        FocusMode.AfLock -> "AF-L"
        FocusMode.Mf -> "MF"
    }
    val valueText = state.focusDistanceLabel()
    val locked = mode != FocusMode.Af

    var dragging by remember { mutableStateOf(false) }
    var position by remember { mutableStateOf(FocusScrubPosition(0f)) }

    CompactParamShell(
        icon = Icons.Outlined.CenterFocusStrong,
        title = title,
        valueText = valueText,
        locked = locked,
        testTag = CaptureTestTags.COMPACT_FOCUS,
        semanticsDescription = "Focus $title $valueText. " +
            if (ladder != null) {
                "Scrub horizontally or vertically for manual focus, right or up is farther. " +
                    "Long press to lock autofocus, double tap for continuous autofocus."
            } else {
                "Manual focus unavailable. Double tap for continuous autofocus."
            },
        modifier = modifier
            .then(if (ladder == null) Modifier.graphicsLayer(alpha = .45f) else Modifier)
            .pointerInput(state.capabilities.tapAfSupported) {
                detectTapGestures(
                    onDoubleTap = {
                        if (!dragging && latestMode != FocusMode.Af) {
                            haptics.selection()
                            dispatch(SetFocusMode(FocusMode.Af))
                        }
                    },
                    onLongPress = {
                        if (dragging) return@detectTapGestures
                        haptics.longPress()
                        if (latestMode == FocusMode.Af && state.capabilities.tapAfSupported) {
                            dispatch(SetFocusMode(FocusMode.AfLock))
                        }
                    }
                )
            }
            .then(
                if (ladder != null) {
                    Modifier.pointerInput(state.orientation, ladder) {
                        detectDragGestures(
                            onDragStart = {
                                dragging = true
                                // Exact current lens position: entering MF never moves focus.
                                position = ladder.start(latestSeedDiopters)
                                if (latestMode != FocusMode.Mf) {
                                    haptics.selection()
                                    dispatch(SetManualFocus(ladder.normalizedOf(position.diopters)))
                                }
                            },
                            onDrag = { change, drag ->
                                change.consume()
                                val (next, landed) = ladder.scrub(position, (drag.x - drag.y).toDp().value)
                                if (next.diopters != position.diopters) {
                                    dispatch(SetManualFocus(ladder.normalizedOf(next.diopters)))
                                }
                                if (landed) haptics.detent()
                                position = next
                            },
                            onDragEnd = { dragging = false },
                            onDragCancel = { dragging = false }
                        )
                    }
                } else {
                    Modifier
                }
            )
    )
}
