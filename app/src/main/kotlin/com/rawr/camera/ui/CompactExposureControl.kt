package com.rawr.camera.ui

import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.size
import com.rawr.camera.ui.icons.Icons
import com.rawr.camera.ui.icons.outlined.Exposure
import com.rawr.camera.ui.icons.outlined.Iso
import com.rawr.camera.ui.icons.outlined.ShutterSpeed
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.testTag
import com.rawr.camera.architecture.CaptureDispatch
import com.rawr.camera.architecture.CaptureTransitions
import com.rawr.camera.architecture.ScrubRenderExposure
import com.rawr.camera.architecture.SetExposureCandidate
import com.rawr.camera.architecture.SetExposureMode
import com.rawr.camera.model.CaptureMode
import com.rawr.camera.model.CaptureUiState
import com.rawr.camera.model.ExposureMode
import com.rawr.camera.model.ExposureParameter
import com.rawr.camera.model.TonemapControlContract
import com.rawr.camera.model.capabilityFor
import com.rawr.camera.model.labelsAround
import com.rawr.camera.model.requestedCandidateIdFor
import com.rawr.camera.model.requestedCandidateIndexFor
import com.rawr.camera.model.zeroEvCandidateId

@Composable
internal fun CompactExposureButton(
    parameter: ExposureParameter,
    state: CaptureUiState,
    dispatch: CaptureDispatch,
    modifier: Modifier = Modifier
) {
    if (parameter == ExposureParameter.Ev && CaptureTransitions.evAdjustsRenderExposure(state)) {
        CompactRenderExposureButton(state, dispatch, modifier)
        return
    }
    val capability = state.capabilities.capabilityFor(parameter)
    val requestedId = state.requestedCandidateIdFor(parameter)
    val index = state.requestedCandidateIndexFor(parameter)
    val latestRequestedId by rememberUpdatedState(requestedId)
    val latestMode by rememberUpdatedState(state.exposureControl.mode)
    val haptics = LocalCaptureHaptics.current
    val isEv = parameter == ExposureParameter.Ev
    val isManualMeter = isEv && state.exposureControl.mode == ExposureMode.Manual
    val locked = CaptureTransitions.isParameterLocked(state, parameter)
    val zeroEvId = if (isEv) capability.zeroEvCandidateId() else null
    val zeroEvIndex = if (isEv && zeroEvId != null) capability.indexOf(zeroEvId).takeIf { it >= 0 } else null
    val interactive = !isManualMeter
    // Live backend readout for this axis. While the axis is auto, the button
    // mirrors it (same source as ExposureMonitor, so the two can never drift
    // apart); the requested value is only shown once locked.
    val applied = when (parameter) {
        ExposureParameter.Shutter -> state.exposureApplied.shutter
        ExposureParameter.Iso -> state.exposureApplied.iso
        ExposureParameter.Ev -> state.exposureApplied.evOrMeter
    }
    val showApplied = !locked && !isEv
    val seedId =
        if (locked || isEv) {
            requestedId
        } else {
            applied?.sourceCandidateId?.takeIf { capability.candidate(it) != null } ?: requestedId
        }
    val seedIndex = capability.indexOf(seedId).takeIf { it >= 0 } ?: index
    val latestSeedIndex by rememberUpdatedState(seedIndex)
    val requestedLabels = capability.labelsAround(requestedId)
    // In Video the shutter control always speaks in shutter angles, even
    // while unlocked (Auto): show the nearest preset to the live exposure.
    // The ExposureMonitor keeps the exact 1/N telemetry; this button is the
    // control, so it follows the control convention (degrees).
    val isVideoShutter =
        parameter == ExposureParameter.Shutter && state.captureMode == CaptureMode.Video
    val appliedAngleLabel =
        if (isVideoShutter) {
            applied?.sourceCandidateId?.let { capability.candidate(it)?.displayLabel }
        } else {
            null
        }
    val valueText = when {
        isManualMeter -> applied?.displayLabel ?: "—"
        showApplied && isVideoShutter -> appliedAngleLabel ?: requestedLabels.current
        showApplied -> applied?.displayLabel ?: requestedLabels.current
        else -> requestedLabels.current
    }

    var rawTrackOffsetPx by remember(parameter) { mutableFloatStateOf(0f) }
    var dragging by remember(parameter) { mutableStateOf(false) }
    var gestureIndex by remember(parameter) { mutableIntStateOf(index) }

    val icon = when (parameter) {
        ExposureParameter.Shutter -> Icons.Outlined.ShutterSpeed
        ExposureParameter.Iso -> Icons.Outlined.Iso
        // EV keeps the Exposure glyph (labels + values disambiguate it from ISO).
        ExposureParameter.Ev -> Icons.Outlined.Exposure
    }
    val testTag = when (parameter) {
        ExposureParameter.Shutter -> CaptureTestTags.COMPACT_SS
        ExposureParameter.Iso -> CaptureTestTags.COMPACT_ISO
        ExposureParameter.Ev -> CaptureTestTags.COMPACT_EV
    }
    val title = when (parameter) {
        ExposureParameter.Shutter -> "SS"
        ExposureParameter.Iso -> "ISO"
        ExposureParameter.Ev -> "EV"
    }

    fun ensureLocked() {
        if (isEv || isManualMeter) return
        CaptureTransitions.lockModeFor(parameter, latestMode)?.let {
            haptics.selection()
            dispatch(SetExposureMode(it))
        }
    }

    CompactParamShell(
        icon = icon,
        title = title,
        valueText = valueText,
        locked = locked && !isManualMeter,
        testTag = testTag,
        semanticsDescription = compactExposureDescription(title, valueText, locked, isManualMeter),
        modifier = modifier
            .then(
                if (isEv && zeroEvId != null && zeroEvIndex != null && !isManualMeter) {
                    Modifier.pointerInput(capability, zeroEvId) {
                        detectTapGestures(
                            onDoubleTap = {
                                if (!dragging) {
                                    haptics.selection()
                                    gestureIndex = zeroEvIndex
                                    rawTrackOffsetPx = 0f
                                    if (latestRequestedId != zeroEvId) {
                                        dispatch(SetExposureCandidate(parameter, zeroEvId))
                                    }
                                }
                            },
                            onLongPress = {
                                // EV has no auto/manual mode distinction: long-press
                                // is a haptic confirm only, never a mode change.
                                if (!dragging) haptics.longPress()
                            }
                        )
                    }
                } else if (!isEv && interactive) {
                    Modifier.pointerInput(parameter, capability.candidates.size) {
                        detectTapGestures(
                            onDoubleTap = {
                                if (!dragging) {
                                    CaptureTransitions.unlockModeFor(parameter, latestMode)?.let {
                                        haptics.selection()
                                        dispatch(SetExposureMode(it))
                                    }
                                }
                            },
                            onLongPress = {
                                if (!dragging) {
                                    CaptureTransitions.lockModeFor(parameter, latestMode)?.let {
                                        haptics.longPress()
                                        dispatch(SetExposureMode(it))
                                    } ?: haptics.longPress()
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
                    Modifier.pointerInput(state.orientation, parameter, capability.candidates.size) {
                        val detentPx = CaptureDimens.ExposureDetentSpacing.toPx()
                        val magnetic = capability.magneticSnapAnchorIds.isNotEmpty()
                        val fineStepPx = if (magnetic) detentPx / 8f else detentPx
                        val magneticReleasePx = if (magnetic) fineStepPx * 2.25f else fineStepPx
                        detectDragGestures(
                            onDragStart = {
                                dragging = true
                                gestureIndex = latestSeedIndex
                                rawTrackOffsetPx = 0f
                                ensureLocked()
                            },
                            onDrag = { change, drag ->
                                change.consume()
                                // Vertical is an alternate axis for the same
                                // value (up = increase): edge columns run out
                                // of horizontal room, vertical never does.
                                // Pure-horizontal feel is unchanged.
                                val delta = drag.x - drag.y
                                var residual = rawTrackOffsetPx + delta
                                while (gestureIndex < capability.candidates.lastIndex) {
                                    val currentIsMagnetic =
                                        capability.candidates[gestureIndex].id in capability.magneticSnapAnchorIds
                                    val threshold = if (currentIsMagnetic) magneticReleasePx else fineStepPx
                                    if (residual < threshold) break
                                    gestureIndex += 1
                                    val candidate = capability.candidates[gestureIndex]
                                    dispatch(SetExposureCandidate(parameter, candidate.id))
                                    if (!magnetic || candidate.id in capability.magneticSnapAnchorIds) haptics.detent()
                                    residual -= threshold
                                }
                                while (gestureIndex > 0) {
                                    val currentIsMagnetic =
                                        capability.candidates[gestureIndex].id in capability.magneticSnapAnchorIds
                                    val threshold = if (currentIsMagnetic) magneticReleasePx else fineStepPx
                                    if (residual > -threshold) break
                                    gestureIndex -= 1
                                    val candidate = capability.candidates[gestureIndex]
                                    dispatch(SetExposureCandidate(parameter, candidate.id))
                                    if (!magnetic || candidate.id in capability.magneticSnapAnchorIds) haptics.detent()
                                    residual += threshold
                                }
                                val pushingPastMin = gestureIndex == 0 && residual < 0f
                                val pushingPastMax =
                                    gestureIndex == capability.candidates.lastIndex && residual > 0f
                                rawTrackOffsetPx =
                                    if (pushingPastMin || pushingPastMax) residual * .18f else residual
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
    )
}

/**
 * Full-Manual EV button: SS and ISO are both fixed, so the camera EV axis is a
 * read-only meter. The same button (icon and title unchanged) instead scrubs
 * the active profile's render exposure in 0.1 EV steps. Double-tap resets to
 * +0.0; long-press is haptic-only, like camera EV.
 */
@Composable
private fun CompactRenderExposureButton(
    state: CaptureUiState,
    dispatch: CaptureDispatch,
    modifier: Modifier = Modifier
) {
    val haptics = LocalCaptureHaptics.current
    val tenths = state.renderExposureTenths
    val latestTenths by rememberUpdatedState(tenths)
    var dragging by remember { mutableStateOf(false) }
    val valueText = renderExposureLabel(tenths)
    CompactParamShell(
        icon = Icons.Outlined.Exposure,
        title = "EV",
        valueText = valueText,
        locked = tenths != 0,
        testTag = CaptureTestTags.COMPACT_EV,
        semanticsDescription = "EV render exposure $valueText. Scrub horizontally or vertically to adjust. " +
            "Double tap to reset to zero.",
        modifier = modifier
            .pointerInput(Unit) {
                detectTapGestures(
                    onDoubleTap = {
                        if (!dragging && latestTenths != 0) {
                            haptics.selection()
                            dispatch(ScrubRenderExposure(-latestTenths))
                        }
                    },
                    onLongPress = { if (!dragging) haptics.longPress() }
                )
            }
            .pointerInput(state.orientation) {
                val stepPx = CaptureDimens.ExposureDetentSpacing.toPx() / 3f
                var gesture = 0
                var residual = 0f
                detectDragGestures(
                    onDragStart = {
                        dragging = true
                        gesture = latestTenths
                        residual = 0f
                    },
                    onDrag = { change, drag ->
                        change.consume()
                        // Same axes as the camera EV button: right/up = brighter.
                        residual += drag.x - drag.y
                        val steps = (residual / stepPx).toInt()
                        if (steps == 0) return@detectDragGestures
                        residual -= steps * stepPx
                        val next = (gesture + steps).coerceIn(
                            TonemapControlContract.EXPOSURE_MIN_TENTHS,
                            TonemapControlContract.EXPOSURE_MAX_TENTHS
                        )
                        if (next == gesture) {
                            residual = 0f
                            return@detectDragGestures
                        }
                        // Detents on every half stop keep the 0.1 EV scrub from buzzing.
                        val crossedHalfStop = (minOf(gesture, next) + 1..maxOf(gesture, next)).any { it % 5 == 0 }
                        dispatch(ScrubRenderExposure(next - gesture))
                        gesture = next
                        if (crossedHalfStop) haptics.detent()
                    },
                    onDragEnd = { dragging = false },
                    onDragCancel = { dragging = false }
                )
            }
    )
}

internal fun renderExposureLabel(tenths: Int): String {
    val sign = if (tenths < 0) "-" else "+"
    val abs = kotlin.math.abs(tenths)
    return "$sign${abs / 10}.${abs % 10}"
}

private fun compactExposureDescription(
    title: String,
    value: String,
    locked: Boolean,
    isMeter: Boolean
): String {
    // Primary axis is horizontal (activity stays portrait-locked); vertical
    // drag mirrors it (up = increase) for edge columns with no outward room.
    val axis = "horizontally or vertically"
    return when {
        isMeter -> "$title meter $value. Read-only in manual mode."
        title == "EV" -> "$title $value, ${if (locked) "active" else "auto"}. Scrub $axis to adjust. Double tap to reset to zero."
        else -> "$title $value, ${if (locked) "locked" else "auto"}. Scrub $axis to adjust and lock. Long press to lock, double tap for auto."
    }
}
