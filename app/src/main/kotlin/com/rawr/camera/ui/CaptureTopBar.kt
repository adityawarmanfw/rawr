package com.rawr.camera.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import com.rawr.camera.ui.icons.Icons
import com.rawr.camera.ui.icons.outlined.AspectRatio
import com.rawr.camera.ui.icons.outlined.Hd
import com.rawr.camera.ui.icons.outlined.HighQuality
import com.rawr.camera.ui.icons.outlined.Settings
import com.rawr.camera.ui.icons.outlined.SlowMotionVideo
import com.rawr.camera.ui.icons.outlined.Thermostat
import com.rawr.camera.ui.icons.outlined.Timer
import com.rawr.camera.ui.icons.outlined.TimerOff
import com.rawr.camera.ui.icons.outlined.Visibility
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.toggleableState
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.state.ToggleableState
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.rawr.camera.architecture.CancelSelfTimer
import com.rawr.camera.architecture.CaptureDispatch
import com.rawr.camera.architecture.CycleOutputFormat
import com.rawr.camera.architecture.CycleSelfTimer
import com.rawr.camera.architecture.CycleVideoFps
import com.rawr.camera.architecture.CycleVideoResolution
import com.rawr.camera.architecture.ToggleMonitorPanel
import com.rawr.camera.architecture.ToggleWhiteBalancePanel
import com.rawr.camera.model.CaptureMode
import com.rawr.camera.model.CaptureUiState
import com.rawr.camera.model.WhiteBalanceMode
import com.rawr.camera.settings.model.SelfTimer
import com.rawr.camera.video.VideoResolutionMode

@Composable
internal fun CaptureTopBar(
    state: CaptureUiState,
    dispatch: CaptureDispatch,
    onOpenSettings: () -> Unit,
    videoLocked: Boolean = false
) {
    val haptics = LocalCaptureHaptics.current
    Row(
        Modifier
            .fillMaxWidth()
            .background(CaptureColors.Surface)
            .height(CaptureDimens.TopBarHeight),
        verticalAlignment = Alignment.CenterVertically
    ) {
        if (state.captureMode == CaptureMode.Video) {
            TopBarSlot {
                VideoTopBarChoice(
                    icon = state.videoResolution.topBarIcon(),
                    label = state.videoResolution.topBarLabel(),
                    testTag = CaptureTestTags.TOPBAR_VIDEO_RESOLUTION,
                    description = "Video resolution ${state.videoResolution.label}. Tap to change.",
                    enabled = !videoLocked,
                    orientation = state.orientation,
                    rotateLabel = false,
                    onClick = {
                        haptics.selection()
                        dispatch(CycleVideoResolution)
                    }
                )
            }
            TopBarSlot {
                VideoTopBarChoice(
                    icon = Icons.Outlined.SlowMotionVideo,
                    label = "${state.videoFps}",
                    testTag = CaptureTestTags.TOPBAR_VIDEO_FPS,
                    description = "Video frame rate ${state.videoFps} fps. Tap to change.",
                    enabled = !videoLocked,
                    orientation = state.orientation,
                    onClick = {
                        haptics.selection()
                        dispatch(CycleVideoFps)
                    }
                )
            }
            TopBarSlot {
                TextButton(
                    onClick = { haptics.selection(); dispatch(com.rawr.camera.architecture.ToggleVideoLog) },
                    enabled = !videoLocked,
                    modifier = Modifier.size(48.dp).testTag(CaptureTestTags.TOPBAR_VIDEO_LOG)
                        .semantics {
                            contentDescription = "LOG recording"
                            role = Role.Switch
                            toggleableState = ToggleableState(state.videoLogEnabled)
                        },
                    contentPadding = PaddingValues(0.dp)
                ) {
                    Text("LOG", fontFamily = CaptureMono, fontSize = 11.sp, fontWeight = FontWeight.Bold,
                        color = if (state.videoLogEnabled) CaptureColors.Accent else CaptureColors.Muted,
                        modifier = Modifier.uprightInLandscape(state.orientation))
                }
            }
            TopBarSlot {
                SelfTimerTopBarChoice(
                    state = state,
                    dispatch = dispatch,
                    orientation = state.orientation
                )
            }
        } else {
            TopBarSlot {
                // A DNG-only (200 MP) lens pins the button to DNG; the saved choice returns on other lenses.
                val dngOnly = state.capabilities.dngOnly
                val format = com.rawr.camera.model.CaptureOutputFormat.from(state.dngEnabled, state.jpegEnabled, dngOnly)
                TextButton(
                    onClick = {
                        haptics.selection()
                        dispatch(CycleOutputFormat)
                    },
                    enabled = !dngOnly,
                    modifier = Modifier.size(48.dp).testTag(CaptureTestTags.TOPBAR_JPEG)
                        .semantics {
                            contentDescription = if (dngOnly) "Output format: DNG only on this lens"
                            else "Output format: ${format.label}. Tap to cycle formats"
                        },
                    contentPadding = PaddingValues(0.dp)
                ) {
                    Text(
                        when (format) {
                            com.rawr.camera.model.CaptureOutputFormat.Dng -> "DNG"
                            com.rawr.camera.model.CaptureOutputFormat.DngAndJpeg -> "DNG +\nJPEG"
                            com.rawr.camera.model.CaptureOutputFormat.Jpeg -> "JPEG"
                        },
                        textAlign = androidx.compose.ui.text.style.TextAlign.Center,
                        lineHeight = 13.sp,
                        color = CaptureColors.Accent,
                        fontFamily = CaptureMono,
                        fontSize = 11.sp,
                        fontWeight = FontWeight.Bold,
                        modifier = Modifier.uprightInLandscape(state.orientation)
                    )
                }
            }
            TopBarSlot {
                SelfTimerTopBarChoice(
                    state = state,
                    dispatch = dispatch,
                    orientation = state.orientation
                )
            }
        }
        TopBarSlot {
            IconToggleButton(
                checked =
                    state.whiteBalancePanelOpen ||
                        state.whiteBalanceMode != WhiteBalanceMode.Auto,
                onCheckedChange = {
                    haptics.selection()
                    dispatch(ToggleWhiteBalancePanel)
                },
                modifier = Modifier.testTag(CaptureTestTags.TOPBAR_WHITE_BALANCE)
                    .semantics { contentDescription = "White balance" },
                colors =
                    IconButtonDefaults.iconToggleButtonColors(
                        contentColor = CaptureColors.Muted,
                        checkedContentColor = CaptureColors.Accent
                    )
            ) {
                Icon(
                    Icons.Outlined.Thermostat,
                    contentDescription = null,
                    modifier = Modifier.size(22.dp).uprightInLandscape(state.orientation)
                )
            }
        }
        TopBarSlot {
            IconToggleButton(
                checked = state.monitorPanelOpen || state.armedOverlays.isNotEmpty() ||
                    state.falseColorManual || state.activeScopes.isNotEmpty(),
                onCheckedChange = {
                    haptics.selection()
                    dispatch(ToggleMonitorPanel)
                },
                modifier = Modifier.testTag(CaptureTestTags.TOPBAR_MONITOR)
                    .semantics { contentDescription = "Monitoring and overlays" },
                colors =
                    IconButtonDefaults.iconToggleButtonColors(
                        contentColor = CaptureColors.Muted,
                        checkedContentColor = CaptureColors.Accent
                    )
            ) {
                Icon(
                    Icons.Outlined.Visibility,
                    contentDescription = null,
                    modifier = Modifier.size(22.dp).uprightInLandscape(state.orientation)
                )
            }
        }
        TopBarSlot {
            IconButton(
                enabled = !videoLocked,
                onClick = {
                    haptics.selection()
                    onOpenSettings()
                },
                modifier = Modifier.testTag(CaptureTestTags.TOPBAR_SETTINGS)
                    .semantics { contentDescription = "Open Settings" },
                colors = IconButtonDefaults.iconButtonColors(contentColor = CaptureColors.Muted.copy(alpha = .82f))
            ) {
                Icon(
                    Icons.Outlined.Settings,
                    contentDescription = null,
                    modifier = Modifier.size(21.dp).uprightInLandscape(state.orientation)
                )
            }
        }
    }
}

private fun VideoResolutionMode.topBarIcon(): ImageVector = when (this) {
    VideoResolutionMode.HD1080 -> Icons.Outlined.Hd
    VideoResolutionMode.UHD4K -> Icons.Outlined.HighQuality
    VideoResolutionMode.OPEN_GATE -> Icons.Outlined.AspectRatio
}

private fun VideoResolutionMode.topBarLabel(): String = when (this) {
    VideoResolutionMode.HD1080 -> "1080p"
    VideoResolutionMode.UHD4K -> "4K"
    VideoResolutionMode.OPEN_GATE -> "OPEN"
}

@Composable
private fun VideoTopBarChoice(
    icon: ImageVector,
    label: String,
    testTag: String,
    description: String,
    enabled: Boolean,
    orientation: com.rawr.camera.model.Orientation,
    rotateLabel: Boolean = true,
    onClick: () -> Unit
) {
    TextButton(
        onClick = onClick,
        enabled = enabled,
        modifier = Modifier.size(48.dp).testTag(testTag)
            .semantics { contentDescription = description },
        contentPadding = PaddingValues(0.dp)
    ) {
        Column(horizontalAlignment = Alignment.CenterHorizontally) {
            Icon(
                icon,
                contentDescription = null,
                tint = if (enabled) CaptureColors.Accent else CaptureColors.Muted,
                modifier = Modifier.size(20.dp).uprightInLandscape(orientation)
            )
            Text(
                label,
                color = if (enabled) CaptureColors.Accent else CaptureColors.Muted,
                fontFamily = CaptureMono,
                fontSize = 8.sp,
                lineHeight = 8.sp,
                fontWeight = FontWeight.Bold,
                maxLines = 1,
                modifier = if (rotateLabel) Modifier.uprightInLandscape(orientation) else Modifier
            )
        }
    }
}

/**
 * Self-timer slot, immediately left of white balance in both modes. Cycles
 * Off → 3s → 5s → 10s; while a countdown runs it shows the remaining whole
 * seconds and a tap cancels instead of cycling.
 */
@Composable
private fun SelfTimerTopBarChoice(
    state: CaptureUiState,
    dispatch: CaptureDispatch,
    orientation: com.rawr.camera.model.Orientation
) {
    val haptics = LocalCaptureHaptics.current
    val remaining = state.selfTimerRemainingMs
    val counting = remaining != null
    val label = if (counting) {
        "${((remaining + 999) / 1000).toInt().coerceAtLeast(1)}s"
    } else {
        state.selfTimer.shortLabel
    }
    val armed = counting || state.selfTimer != SelfTimer.Off
    val description = if (counting) {
        "Self timer counting down $label. Tap to cancel."
    } else {
        "Self timer ${state.selfTimer.label}. Tap to change."
    }
    TextButton(
        onClick = {
            haptics.selection()
            dispatch(if (counting) CancelSelfTimer else CycleSelfTimer)
        },
        modifier = Modifier.size(48.dp).testTag(CaptureTestTags.TOPBAR_SELF_TIMER)
            .semantics { contentDescription = description },
        contentPadding = PaddingValues(0.dp)
    ) {
        Column(horizontalAlignment = Alignment.CenterHorizontally) {
            Icon(
                if (armed) Icons.Outlined.Timer else Icons.Outlined.TimerOff,
                contentDescription = null,
                tint = if (armed) CaptureColors.Accent else CaptureColors.Muted,
                modifier = Modifier.size(20.dp).uprightInLandscape(orientation)
            )
            Text(
                label,
                color = if (armed) CaptureColors.Accent else CaptureColors.Muted,
                fontFamily = CaptureMono,
                fontSize = 8.sp,
                lineHeight = 8.sp,
                fontWeight = FontWeight.Bold,
                maxLines = 1,
                modifier = Modifier.uprightInLandscape(orientation)
            )
        }
    }
}

@Composable
private fun RowScope.TopBarSlot(content: @Composable () -> Unit) {    Box(
        Modifier.weight(1f).fillMaxHeight(),
        contentAlignment = Alignment.Center
    ) {
        content()
    }
}
